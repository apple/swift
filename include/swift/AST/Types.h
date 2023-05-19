//===--- Types.h - Swift Language Type ASTs ---------------------*- C++ -*-===//
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
// This file defines the TypeBase class and subclasses, which describe the Swift
// and SIL ASTs. See also: Type.h.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_TYPES_H
#define SWIFT_TYPES_H

#include "swift/AST/ASTAllocated.h"
#include "swift/AST/AutoDiff.h"
#include "swift/AST/DeclContext.h"
#include "swift/AST/ExtInfo.h"
#include "swift/AST/GenericParamKey.h"
#include "swift/AST/Identifier.h"
#include "swift/AST/Ownership.h"
#include "swift/AST/ProtocolConformanceRef.h"
#include "swift/AST/Requirement.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/AST/Type.h"
#include "swift/AST/TypeAlignments.h"
#include "swift/AST/TypeExpansionContext.h"
#include "swift/Basic/ArrayRefView.h"
#include "swift/Basic/Debug.h"
#include "swift/Basic/InlineBitfield.h"
#include "swift/Basic/UUID.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMapInfo.h"
#include "llvm/ADT/FoldingSet.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/PointerEmbeddedInt.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/TrailingObjects.h"

namespace llvm {
struct fltSemantics;
} // namespace llvm

namespace swift {

enum class AllocationArena;
class ArchetypeType;
class ArgumentList;
class AssociatedTypeDecl;
class ASTContext;
enum BufferPointerTypeKind : unsigned;
class BuiltinTupleDecl;
class ClassDecl;
class ClangModuleLoader;
class DependentMemberType;
class GenericTypeParamDecl;
class GenericTypeParamType;
class GenericParamList;
class GenericSignature;
class GenericSignatureBuilder;
class Identifier;
class InOutType;
class OpaqueTypeDecl;
class OpenedArchetypeType;
class PackExpansionType;
class PackType;
enum class ParamSpecifier : uint8_t;
class PlaceholderTypeRepr;
enum class ReferenceCounting : uint8_t;
enum class ResilienceExpansion : unsigned;
class SILModule;
class SILType;
class TypeAliasDecl;
class TypeDecl;
class NominalTypeDecl;
class GenericTypeDecl;
enum class EffectKind : uint8_t;
class EnumDecl;
class EnumElementDecl;
class SILFunctionType;
class StructDecl;
class ParamDecl;
class ProtocolDecl;
class TypeVariableType;
class ValueDecl;
class ModuleDecl;
class ModuleType;
class ProtocolConformance;
enum PointerTypeKind : unsigned;
struct ValueOwnershipKind;
class ErrorExpr;

typedef CanTypeWrapper<SILFunctionType> CanSILFunctionType;

enum class TypeKind : uint8_t {
#define TYPE(id, parent) id,
#define LAST_TYPE(id) Last_Type = id,
#define TYPE_RANGE(Id, FirstId, LastId) \
  First_##Id##Type = FirstId, Last_##Id##Type = LastId,
#include "swift/AST/TypeNodes.def"
};

enum : unsigned {
  NumTypeKindBits = countBitsUsed(static_cast<unsigned>(TypeKind::Last_Type))
};

enum class BuiltinTypeKind : std::underlying_type<TypeKind>::type;

/// Various properties of types that are primarily defined recursively
/// on structural types.
class RecursiveTypeProperties {
public:
  /// A single property.
  ///
  /// Note that the property polarities should be chosen so that 0 is
  /// the correct default value and bitwise-or correctly merges things.
  enum Property : unsigned {
    /// This type expression contains a TypeVariableType.
    HasTypeVariable      = 0x01,

    /// This type expression contains a context-dependent archetype, either a
    /// \c PrimaryArchetypeType, \c OpenedArchetypeType,
    /// \c ElementArchetypeType, or \c PackArchetype.
    HasArchetype         = 0x02,

    /// This type expression contains a GenericTypeParamType.
    HasTypeParameter     = 0x04,

    /// This type expression contains an UnresolvedType.
    HasUnresolvedType    = 0x08,

    /// Whether this type expression contains an unbound generic type.
    HasUnboundGeneric    = 0x10,

    /// This type expression contains an LValueType other than as a
    /// function input, and can be loaded to convert to an rvalue.
    IsLValue             = 0x20,

    /// This type expression contains an opened existential ArchetypeType.
    HasOpenedExistential = 0x40,

    /// This type expression contains a DynamicSelf type.
    HasDynamicSelf       = 0x80,

    /// This type contains an Error type.
    HasError             = 0x100,

    /// This type contains a DependentMemberType.
    HasDependentMember   = 0x200,

    /// This type contains an OpaqueTypeArchetype.
    HasOpaqueArchetype   = 0x400,

    /// This type contains a type placeholder.
    HasPlaceholder       = 0x800,

    /// This type contains a generic type parameter that is declared as a
    /// parameter pack.
    HasParameterPack = 0x1000,

    /// This type contains a parameterized existential type \c any P<T>.
    HasParameterizedExistential = 0x2000,

    /// This type contains an ElementArchetype.
    HasElementArchetype = 0x4000,

    /// Whether the type is allocated in the constraint solver arena. This can
    /// differ from \c HasTypeVariable for types such as placeholders, which do
    /// not have type variables, but we still want to allocate in the solver if
    /// they have a type variable originator.
    SolverAllocated = 0x8000,

    Last_Property = SolverAllocated
  };
  enum { BitWidth = countBitsUsed(Property::Last_Property) };

private:
  unsigned Bits;

public:
  RecursiveTypeProperties() : Bits(0) {}
  RecursiveTypeProperties(Property prop) : Bits(prop) {}
  explicit RecursiveTypeProperties(unsigned bits) : Bits(bits) {}

  /// Return these properties as a bitfield.
  unsigned getBits() const { return Bits; }

  /// Does a type with these properties structurally contain a type
  /// variable?
  bool hasTypeVariable() const { return Bits & HasTypeVariable; }

  /// Does a type with these properties structurally contain a
  /// context-dependent archetype (that is, a Primary- or OpenedArchetype)?
  bool hasArchetype() const { return Bits & HasArchetype; }

  /// Does a type with these properties structurally contain an
  /// archetype from an opaque type declaration?
  bool hasOpaqueArchetype() const { return Bits & HasOpaqueArchetype; }
  
  /// Does a type with these properties have a type parameter somewhere in it?
  bool hasTypeParameter() const { return Bits & HasTypeParameter; }

  /// Does a type with these properties have an unresolved type somewhere in it?
  bool hasUnresolvedType() const { return Bits & HasUnresolvedType; }

  /// Is a type with these properties an lvalue?
  bool isLValue() const { return Bits & IsLValue; }

  /// Does this type contain an error?
  bool hasError() const { return Bits & HasError; }

  /// Does this type contain a dependent member type, possibly with a
  /// non-type parameter base, such as a type variable or concrete type?
  bool hasDependentMember() const { return Bits & HasDependentMember; }

  /// Does a type with these properties structurally contain an
  /// opened existential archetype?
  bool hasOpenedExistential() const { return Bits & HasOpenedExistential; }

  /// Does a type with these properties structurally contain an
  /// opened element archetype?
  bool hasElementArchetype() const { return Bits & HasElementArchetype; }

  /// Whether the type is allocated in the constraint solver arena. This can
  /// differ from \c hasTypeVariable() for types such as placeholders, which
  /// do not have type variables, but we still want to allocate in the solver if
  /// they have a type variable originator.
  bool isSolverAllocated() const { return Bits & SolverAllocated; }

  /// Does a type with these properties structurally contain a local
  /// archetype?
  bool hasLocalArchetype() const {
    return hasOpenedExistential() || hasElementArchetype();
  }

  /// Does a type with these properties structurally contain a
  /// reference to DynamicSelf?
  bool hasDynamicSelf() const { return Bits & HasDynamicSelf; }

  /// Does a type with these properties structurally contain an unbound
  /// generic type?
  bool hasUnboundGeneric() const { return Bits & HasUnboundGeneric; }

  /// Does a type with these properties structurally contain a placeholder?
  bool hasPlaceholder() const { return Bits & HasPlaceholder; }

  bool hasParameterPack() const { return Bits & HasParameterPack; }

  /// Does a type with these properties structurally contain a
  /// parameterized existential type?
  bool hasParameterizedExistential() const {
    return Bits & HasParameterizedExistential;
  }

  /// Returns the set of properties present in either set.
  friend RecursiveTypeProperties operator|(Property lhs, Property rhs) {
    return RecursiveTypeProperties(unsigned(lhs) | unsigned(rhs));
  }
  friend RecursiveTypeProperties operator|(RecursiveTypeProperties lhs,
                                           RecursiveTypeProperties rhs) {
    return RecursiveTypeProperties(lhs.Bits | rhs.Bits);
  }

  /// Add any properties in the right-hand set to this set.
  RecursiveTypeProperties &operator|=(RecursiveTypeProperties other) {
    Bits |= other.Bits;
    return *this;
  }
  /// Restrict this to only the properties in the right-hand set.
  RecursiveTypeProperties &operator&=(RecursiveTypeProperties other) {
    Bits &= other.Bits;
    return *this;
  }

  /// Remove the HasTypeParameter property from this set.
  void removeHasTypeParameter() {
    Bits &= ~HasTypeParameter;
  }

  /// Remove the HasDependentMember property from this set.
  void removeHasDependentMember() {
    Bits &= ~HasDependentMember;
  }

  /// Test for a particular property in this set.
  bool operator&(Property prop) const {
    return Bits & prop;
  }
};

inline RecursiveTypeProperties operator~(RecursiveTypeProperties::Property P) {
  return RecursiveTypeProperties(~unsigned(P));
}
  
/// The result of a type trait check.
enum class TypeTraitResult {
  /// The type cannot have the trait.
  IsNot,
  /// The generic type can be bound to a type that has the trait.
  CanBe,
  /// The type has the trait irrespective of generic substitutions.
  Is,
};

/// Specifies which normally-unsafe type mismatches should be accepted when
/// checking overrides.
enum class TypeMatchFlags {
  /// Allow properly-covariant overrides.
  AllowOverride = 1 << 0,
  /// Allow a parameter with IUO type to be overridden by a parameter with non-
  /// optional type.
  AllowNonOptionalForIUOParam = 1 << 1,
  /// Allow any mismatches of Optional or ImplicitlyUnwrappedOptional at the
  /// top level of a type.
  ///
  /// This includes function parameters and result types as well as tuple
  /// elements, but excludes generic parameters.
  AllowTopLevelOptionalMismatch = 1 << 2,
  /// Allow any ABI-compatible types to be considered matching.
  AllowABICompatible = 1 << 3,
  /// Allow escaping function parameters to override optional non-escaping ones.
  ///
  /// This is necessary because Objective-C allows optional function parameters
  /// to be non-escaping, but Swift currently does not.
  IgnoreNonEscapingForOptionalFunctionParam = 1 << 4,
  /// Allow compatible opaque archetypes.
  AllowCompatibleOpaqueTypeArchetypes = 1 << 5,
  /// Ignore the @Sendable attributes on functions when matching types.
  IgnoreFunctionSendability = 1 << 6,
};
using TypeMatchOptions = OptionSet<TypeMatchFlags>;

/// Base class for all types which describe the Swift and SIL ASTs.
///
/// See TypeNodes.def for a succinct description of the full class hierarchy.
class alignas(1 << TypeAlignInBits) TypeBase
    : public ASTAllocated<std::aligned_storage<8, 8>::type> {
  
  friend class ASTContext;
  TypeBase(const TypeBase&) = delete;
  void operator=(const TypeBase&) = delete;
  
  /// This union contains to the ASTContext for canonical types, and is
  /// otherwise lazily populated by ASTContext when the canonical form of a
  /// non-canonical type is requested. The disposition of the union is stored
  /// outside of the union for performance. See Bits.TypeBase.IsCanonical.
  union {
    CanType CanonicalType;
    const ASTContext *Context;
  };

  /// Returns true if the given type is a sugared type.
  ///
  /// Only intended for use in compile-time assertions.
  // Specializations of this are at the end of the file.
  template <typename T>
  static constexpr bool isSugaredType() {
    return false;
  }

protected:
  enum { NumAFTExtInfoBits = 11 };
  enum { NumSILExtInfoBits = 11 };
  union { uint64_t OpaqueBits;

  SWIFT_INLINE_BITFIELD_BASE(TypeBase, bitmax(NumTypeKindBits,8) +
                             RecursiveTypeProperties::BitWidth + 1,
    /// Kind - The discriminator that indicates what subclass of type this is.
    Kind : bitmax(NumTypeKindBits,8),

    Properties : RecursiveTypeProperties::BitWidth,

    /// Whether this type is canonical or not.
    IsCanonical : 1
  );

  SWIFT_INLINE_BITFIELD(ErrorType, TypeBase, 1,
    /// Whether there is an original type.
    HasOriginalType : 1
  );

  SWIFT_INLINE_BITFIELD(SugarType, TypeBase, 1,
    HasCachedType : 1
  );

  SWIFT_INLINE_BITFIELD_EMPTY(ParenType, SugarType);

  SWIFT_INLINE_BITFIELD_FULL(AnyFunctionType, TypeBase, NumAFTExtInfoBits+1+1+1+16,
    /// Extra information which affects how the function is called, like
    /// regparm and the calling convention.
    ExtInfoBits : NumAFTExtInfoBits,
    HasExtInfo : 1,
    HasClangTypeInfo : 1,
    HasGlobalActor : 1,
    : NumPadBits,
    NumParams : 16
  );

  SWIFT_INLINE_BITFIELD_FULL(ArchetypeType, TypeBase, 1+1+16,
    HasSuperclass : 1,
    HasLayoutConstraint : 1,
    : NumPadBits,
    NumProtocols : 16
  );

  SWIFT_INLINE_BITFIELD_FULL(TypeVariableType, TypeBase, 7+31,
    /// Type variable options.
    Options : 7,
    : NumPadBits,
    /// The unique number assigned to this type variable.
    ID : 31
  );

  SWIFT_INLINE_BITFIELD(SILFunctionType, TypeBase, NumSILExtInfoBits+1+4+1+2+1+1,
    ExtInfoBits : NumSILExtInfoBits,
    HasClangTypeInfo : 1,
    CalleeConvention : 4,
    HasErrorResult : 1,
    CoroutineKind : 2,
    HasInvocationSubs : 1,
    HasPatternSubs : 1
  );

  SWIFT_INLINE_BITFIELD(AnyMetatypeType, TypeBase, 2,
    /// The representation of the metatype.
    ///
    /// Zero indicates that no representation has been set; otherwise,
    /// the value is the representation + 1
    Representation : 2
  );

  SWIFT_INLINE_BITFIELD_FULL(ProtocolCompositionType, TypeBase, 1+32,
    /// Whether we have an explicitly-stated class constraint not
    /// implied by any of our members.
    HasExplicitAnyObject : 1,

    : NumPadBits,

    /// The number of protocols being composed.
    Count : 32
  );

  SWIFT_INLINE_BITFIELD_FULL(ParameterizedProtocolType, TypeBase, 32,
    /// The number of type arguments.
    ArgCount : 32
  );

  SWIFT_INLINE_BITFIELD_FULL(TupleType, TypeBase, 32,
    : NumPadBits,

    /// The number of elements of the tuple.
    Count : 32
  );

  SWIFT_INLINE_BITFIELD_FULL(PackType, TypeBase, 32,
    : NumPadBits,

    /// The number of elements of the pack.
    Count : 32
  );

  SWIFT_INLINE_BITFIELD_FULL(SILPackType, TypeBase, 1+32,
    /// Whether elements of the pack are addresses.
    ElementIsAddress : 1,

    : NumPadBits,

    /// The number of elements of the pack
    Count : 32
  );

  SWIFT_INLINE_BITFIELD_FULL(BoundGenericType, TypeBase, 32,
    : NumPadBits,

    /// The number of generic arguments.
    GenericArgCount : 32
  );

  SWIFT_INLINE_BITFIELD_FULL(TypeAliasType, SugarType, 1+1,
    : NumPadBits,

    /// Whether we have a parent type.
    HasParent : 1,

    /// Whether we have a substitution map.
    HasSubstitutionMap : 1
  );

  } Bits;

protected:
  TypeBase(TypeKind kind, const ASTContext *CanTypeCtx,
           RecursiveTypeProperties properties)
    : Context(nullptr) {
    Bits.OpaqueBits = 0;
    Bits.TypeBase.Kind = static_cast<unsigned>(kind);
    Bits.TypeBase.IsCanonical = false;
    // If this type is canonical, switch the CanonicalType union to ASTContext.
    if (CanTypeCtx) {
      Bits.TypeBase.IsCanonical = true;
      Context = CanTypeCtx;
    }
    setRecursiveProperties(properties);
  }

  void setRecursiveProperties(RecursiveTypeProperties properties) {
    assert(!(properties.hasTypeVariable() && !properties.isSolverAllocated()) &&
           "type variables must be solver allocated!");
    Bits.TypeBase.Properties = properties.getBits();
    assert(Bits.TypeBase.Properties == properties.getBits() && "Bits dropped!");
  }

public:
  /// getKind - Return what kind of type this is.
  TypeKind getKind() const { return static_cast<TypeKind>(Bits.TypeBase.Kind); }

  /// isCanonical - Return true if this is a canonical type.
  bool isCanonical() const { return Bits.TypeBase.IsCanonical; }
  
  /// hasCanonicalTypeComputed - Return true if we've already computed a
  /// canonical version of this type.
  bool hasCanonicalTypeComputed() const { return !CanonicalType.isNull(); }

private:
  CanType computeCanonicalType();

public:
  /// getCanonicalType - Return the canonical version of this type, which has
  /// sugar from all levels stripped off.
  CanType getCanonicalType() const {
    if (isCanonical())
      return CanType(const_cast<TypeBase*>(this));
    if (hasCanonicalTypeComputed())
      return CanonicalType;
    return const_cast<TypeBase*>(this)->computeCanonicalType();
  }

  /// getReducedType - Stronger canonicalization which folds away equivalent
  /// associated types, or type parameters that have been made concrete.
  CanType getReducedType(GenericSignature sig);

  /// Canonical protocol composition types are minimized only to a certain
  /// degree to preserve ABI compatibility. This routine enables performing
  /// slower, but stricter minimization at need (e.g. redeclaration checking).
  CanType getMinimalCanonicalType(const DeclContext *useDC) const;

  /// Reconstitute type sugar, e.g., for array types, dictionary
  /// types, optionals, etc.
  TypeBase *reconstituteSugar(bool Recursive);

  // If this type is a syntax sugar type, desugar it. Also desugar any nested
  // syntax sugar types.
  TypeBase *getWithoutSyntaxSugar();

  /// getASTContext - Return the ASTContext that this type belongs to.
  ASTContext &getASTContext() const;

  /// isEqual - Return true if these two types are equal, ignoring sugar.
  ///
  /// To compare sugar, check for pointer equality of the underlying TypeBase *
  /// values, obtained by calling getPointer().
  bool isEqual(Type Other) const;
  
  /// getDesugaredType - If this type is a sugared type, remove all levels of
  /// sugar until we get down to a non-sugar type.
  TypeBase *getDesugaredType();
  
  /// If this type is a (potentially sugared) type of the specified kind, remove
  /// the minimal amount of sugar required to get a pointer to the type.
  template <typename T>
  T *getAs() {
    static_assert(!isSugaredType<T>(), "getAs desugars types");
    auto Ty = getDesugaredType();
    SWIFT_ASSUME(Ty != nullptr);
    return dyn_cast<T>(Ty);
  }

  template <typename T>
  bool is() {
    static_assert(!isSugaredType<T>(), "isa desugars types");
    return isa<T>(getDesugaredType());
  }
  
  template <typename T>
  T *castTo() {
    static_assert(!isSugaredType<T>(), "castTo desugars types");
    return cast<T>(getDesugaredType());
  }

  /// getRecursiveProperties - Returns the properties defined on the
  /// structure of this type.
  RecursiveTypeProperties getRecursiveProperties() const {
    return RecursiveTypeProperties(Bits.TypeBase.Properties);
  }

  /// hasReferenceSemantics() - Do objects of this type have reference
  /// semantics?
  bool hasReferenceSemantics();

  /// Is this a nominally uninhabited type, such as 'Never'?
  bool isUninhabited();

  /// Is this an uninhabited type, such as 'Never' or '(Never, Int)'?
  bool isStructurallyUninhabited();
  
  /// Is this the 'Any' type?
  bool isAny();

  /// Is this an existential containing only marker protocols?
  bool isMarkerExistential();

  bool isPlaceholder();

  /// Returns true if this is a move-only type.
  bool isPureMoveOnly();

  /// Does the type have outer parenthesis?
  bool hasParenSugar() const { return getKind() == TypeKind::Paren; }

  /// Are values of this type essentially just class references,
  /// possibly with some sort of additional information?
  ///
  ///   - any of the builtin reference types
  ///   - a class type
  ///   - a bound generic class type
  ///   - a class-bounded archetype type
  ///   - a class-bounded existential type
  ///   - a dynamic Self type
  bool isAnyClassReferenceType();
  
  /// allowsOwnership() - Are variables of this type permitted to have
  /// ownership attributes?
  bool allowsOwnership(const GenericSignatureImpl *sig = nullptr);

  /// Determine whether this type involves a type variable.
  bool hasTypeVariable() const {
    return getRecursiveProperties().hasTypeVariable();
  }

  /// Determine where this type is a type variable or a dependent
  /// member root in a type variable.
  bool isTypeVariableOrMember();

  /// Determine whether this type involves a UnresolvedType.
  bool hasUnresolvedType() const {
    return getRecursiveProperties().hasUnresolvedType();
  }

  /// Determine whether this type involves a \c PlaceholderType.
  bool hasPlaceholder() const {
    return getRecursiveProperties().hasPlaceholder();
  }

  /// Determine whether the type involves a context-dependent archetype.
  bool hasArchetype() const {
    return getRecursiveProperties().hasArchetype();
  }
  
  /// Determine whether the type involves an opened existential archetype.
  bool hasOpenedExistential() const {
    return getRecursiveProperties().hasOpenedExistential();
  }

  /// Determine whether the type involves an opened element archetype.
  bool hasElementArchetype() const {
    return getRecursiveProperties().hasElementArchetype();
  }

  /// Determine whether the type involves a local archetype.
  bool hasLocalArchetype() const {
    return getRecursiveProperties().hasLocalArchetype();
  }

  bool hasParameterPack() const {
    return getRecursiveProperties().hasParameterPack();
  }

  /// Determine whether the type involves a parameterized existential type.
  bool hasParameterizedExistential() const {
    return getRecursiveProperties().hasParameterizedExistential();
  }

  /// Determine whether the type involves the given opened existential
  /// archetype.
  bool hasOpenedExistentialWithRoot(const OpenedArchetypeType *root) const;

  /// Determine whether the type involves an opaque type.
  bool hasOpaqueArchetype() const {
    return getRecursiveProperties().hasOpaqueArchetype();
  }

  /// Determine whether the type is an opened existential type.
  ///
  /// To determine whether there is an opened existential type
  /// anywhere in the type, use \c hasOpenedExistential.
  bool isOpenedExistential() const;

  /// Determine whether the type is an opened existential type with Error inside
  bool isOpenedExistentialWithError();

  /// Retrieve the set of root opened archetypes that occur within this type.
  void getRootOpenedExistentials(
      SmallVectorImpl<OpenedArchetypeType *> &rootOpenedArchetypes) const;

  /// Retrieve the set of type parameter packs that occur within this type.
  void getTypeParameterPacks(
      SmallVectorImpl<Type> &rootParameterPacks);

  /// Replace opened archetypes with the given root with their most
  /// specific non-dependent upper bounds throughout this type.
  Type typeEraseOpenedArchetypesWithRoot(const OpenedArchetypeType *root,
                                         const DeclContext *useDC) const;

  /// Given a declaration context, returns a function type with the 'self'
  /// type curried as the input if the declaration context describes a type.
  /// Otherwise, returns the type itself.
  Type addCurriedSelfType(const DeclContext *dc);

  /// Map a contextual type to an interface type.
  Type mapTypeOutOfContext();

  /// Compute and return the set of type variables that occur within this
  /// type.
  ///
  /// \param typeVariables This vector is populated with the set of
  /// type variables referenced by this type.
  void getTypeVariables(SmallPtrSetImpl<TypeVariableType *> &typeVariables);

  /// Determine whether this type is a type parameter, which is either a
  /// GenericTypeParamType or a DependentMemberType.
  ///
  /// Note that this routine will return \c false for types that include type
  /// parameters in nested positions, e.g, \c T is a type parameter but
  /// \c X<T> is not a type parameter. Use \c hasTypeParameter to determine
  /// whether a type parameter exists at any position.
  bool isTypeParameter();

  /// Determine whether this type is a type parameter pack, which is
  /// either a GenericTypeParamType or a DependentMemberType.
  ///
  /// Like \c isTypeParameter, this routine will return \c false for types that
  /// include type parameters in nested positions e.g. \c X<T...>.
  bool isParameterPack();

  /// Determine whether this type can dynamically be an optional type.
  ///
  /// \param includeExistential Whether an existential type should be considered
  /// such a type.
  bool canDynamicallyBeOptionalType(bool includeExistential);

  /// Determine whether this type contains a type parameter somewhere in it.
  bool hasTypeParameter() const {
    return getRecursiveProperties().hasTypeParameter();
  }

  /// Find any unresolved dependent member type within this type.
  ///
  /// "Unresolved" dependent member types have no known associated type,
  /// and are only used transiently in the type checker.
  DependentMemberType *findUnresolvedDependentMemberType();

  /// Return the root generic parameter of this type parameter type.
  GenericTypeParamType *getRootGenericParam();

  /// Given that this type is the result of substituting a pack parameter,
  /// return it as a the pack type.  We want to have a representational
  /// invariant that these substitutions always produce PackTypes; when
  /// that's in place, we can replace all uses of this function with
  /// `->castTo<PackType>()`.  In the meantime, it's permitted for these
  /// substitutions to be unadorned pack parameters or archetypes, which
  /// this function will wrap into a pack containing a singleton expansion.
  PackType *getPackSubstitutionAsPackType();

  /// Determines whether this type is an lvalue. This includes both straight
  /// lvalue types as well as tuples or optionals of lvalues.
  bool hasLValueType() {
    return getRecursiveProperties().isLValue();
  }
  
  /// Is this a first-class value type, meaning it is not an LValueType or an
  /// InOutType.
  bool isMaterializable();

  /// Is this a non-escaping type, that is, a non-escaping function type or a
  /// tuple type containing a non-escaping type?
  bool isNoEscape() const;

  /// Determine whether the type is dependent on DynamicSelf.
  bool hasDynamicSelfType() const {
    return getRecursiveProperties().hasDynamicSelf();
  }

  /// Determine whether the type contains an unbound generic type.
  bool hasUnboundGenericType() const {
    return getRecursiveProperties().hasUnboundGeneric();
  }

  /// Determine whether this type contains an error type.
  bool hasError() const {
    return getRecursiveProperties().hasError();
  }

  /// Does this type contain a dependent member type, possibly with a
  /// non-type parameter base, such as a type variable or concrete type?
  bool hasDependentMember() const {
    return getRecursiveProperties().hasDependentMember();
  }

  /// Whether this type represents a generic constraint.
  bool isConstraintType() const;

  /// isExistentialType - Determines whether this type is an existential type,
  /// whose real (runtime) type is unknown but which is known to conform to
  /// some set of protocols. Protocol and protocol-conformance types are
  /// existential types.
  bool isExistentialType();

  /// isAnyExistentialType - Determines whether this type is any kind of
  /// existential type: a protocol type, a protocol composition type, or
  /// an existential metatype.
  bool isAnyExistentialType();

  /// isObjCExistentialType - Determines whether this type is an
  /// class-bounded existential type whose required conformances are
  /// all @objc.  Such types are compatible with ObjC.
  bool isObjCExistentialType();

  // // Is this an ObjC generic class.
  bool isTypeErasedGenericClassType();

  /// Determines whether this type is an existential type with a class protocol
  /// bound.
  bool isClassExistentialType();

  /// Opens an existential instance or meta-type and returns the opened type.
  Type openAnyExistentialType(OpenedArchetypeType *&opened,
                              GenericSignature parentSig);

  /// Break an existential down into a set of constraints.
  ExistentialLayout getExistentialLayout();

  /// If this is an actor or distributed type, get the nominal type declaration
  /// for the actor.
  NominalTypeDecl *getAnyActor();

  /// Determines whether this type is an actor type.
  bool isActorType();

  /// Determines whether this type conforms or inherits (if it's a protocol
  /// type) from `DistributedActor`.
  bool isDistributedActor();

  /// Determine if the type in question is an Array<T> and, if so, provide the
  /// element type of the array.
  Type isArrayType();

  /// Determines the element type of a known
  /// [Autoreleasing]Unsafe[Mutable][Raw]Pointer variant, or returns null if the
  /// type is not a pointer.
  Type getAnyPointerElementType(PointerTypeKind &PTK);
  Type getAnyPointerElementType() {
    PointerTypeKind Ignore;
    return getAnyPointerElementType(Ignore);
  }

  /// Returns a type representing a pointer to \c this.
  ///
  /// \p kind must not be a raw pointer kind, since that would discard the
  /// current type.
  Type wrapInPointer(PointerTypeKind kind);
  
  /// Determines the element type of a known Unsafe[Mutable][Raw]BufferPointer
  /// variant, or returns null if the type is not a buffer pointer.
  Type getAnyBufferPointerElementType(BufferPointerTypeKind &BPTK);
  Type getAnyBufferPointerElementType() {
    BufferPointerTypeKind Ignore;
    return getAnyBufferPointerElementType(Ignore);
  }

  /// Determine whether the given type is "specialized", meaning that
  /// it involves generic types for which generic arguments have been provided.
  /// For example, the types Vector<Int> and Vector<Int>.Element are both
  /// specialized, but the type Vector is not.
  bool isSpecialized();

  /// Determine whether this type is a legal, lowered SIL type.
  ///
  /// A type is SIL-illegal if it is:
  ///   - an l-value type,
  ///   - a metatype without a representation,
  ///   - an AST function type (i.e. subclasses of AnyFunctionType),
  ///   - an optional whose object type is SIL-illegal, or
  ///   - a tuple type with a SIL-illegal element type.
  bool isLegalSILType();

  /// Determine whether this type is a legal formal type.
  ///
  /// A type is illegal as a formal type if it is:
  ///   - an l-value type,
  ///   - a reference storage type,
  ///   - a metatype with a representation,
  ///   - a lowered function type (i.e. SILFunctionType),
  ///   - an optional whose object type is not a formal type, or
  ///   - a tuple type with an element that is not a formal type.
  ///
  /// These are the types of the Swift type system.
  bool isLegalFormalType();

  /// Check if this type is equal to the empty tuple type.
  bool isVoid();

  #define KNOWN_STDLIB_TYPE_DECL(NAME, DECL_CLASS, NUM_GENERIC_PARAMS) \
  /** Check if this type is equal to Swift.NAME. */ \
  bool is##NAME();
  #include "swift/AST/KnownStdlibTypes.def"

  /// Check if this type is equal to Builtin.IntN.
  bool isBuiltinIntegerType(unsigned bitWidth);
  
  /// Check if this is a nominal type defined at the top level of the Swift module
  bool isStdlibType();

  /// Check if this is a CGFloat type from CoreGraphics framework
  /// on macOS or Foundation on Linux.
  bool isCGFloat();

  /// Check if this is either an Array, Set or Dictionary collection type defined
  /// at the top level of the Swift module
  bool isKnownStdlibCollectionType();

  /// If this is a class type or a bound generic class type, returns the
  /// (possibly generic) class.
  ClassDecl *getClassOrBoundGenericClass() const;

  /// If this is a struct type or a bound generic struct type, returns
  /// the (possibly generic) class.
  StructDecl *getStructOrBoundGenericStruct();
  
  /// If this is an enum or a bound generic enum type, returns the
  /// (possibly generic) enum.
  EnumDecl *getEnumOrBoundGenericEnum();

  /// If this is a class, check if this class is a foreign reference type.
  bool isForeignReferenceType();

  /// Determine whether this type may have a superclass, which holds for
  /// classes, bound generic classes, and archetypes that are only instantiable
  /// with a class type.
  bool mayHaveSuperclass();

  /// Determine whether this type satisfies a class layout constraint, written
  /// `T: AnyObject` in the source.
  ///
  /// A class layout constraint is satisfied when we have a single retainable
  /// pointer as the representation, which includes:
  /// - @objc existentials
  /// - class constrained archetypes
  /// - classes
  bool satisfiesClassConstraint();

  /// Determine whether this type can be used as a base type for AST
  /// name lookup, which is the case for nominal types, existential types,
  /// archetypes, and tuples.
  ///
  /// Generally, the static vs instance and mutating vs nonmutating distinction
  /// is handled elsewhere, so metatypes, lvalue types and inout types are not
  /// allowed here.
  ///
  /// Tuples have formal members to project elements by index or by label; these
  /// are handled directly by Sema and do not go through name lookup.
  ///
  /// Bona fide members on tuples are defined on extensions of
  /// Builtin.TheTupleType.
  bool mayHaveMembers() {
    return (is<ArchetypeType>() ||
            is<ModuleType>() ||
            isExistentialType() ||
            getAnyNominal() ||
            is<TupleType>());
  }

  /// Checks whether this type may potentially be callable. This returns true
  /// for function types, metatypes, nominal types that support being called,
  /// and types that have not been inferred yet.
  bool mayBeCallable(DeclContext *dc);

  /// Checks whether this is a type that supports being called through the
  /// implementation of a \c callAsFunction method. Note that this does not
  /// check access control.
  bool isCallAsFunctionType(DeclContext *dc);

  /// Return true if the specified type or a super-class/super-protocol has the
  /// @dynamicMemberLookup attribute on it.
  bool hasDynamicMemberLookupAttribute();
  
  /// Return true if the specified type or a super-class/super-protocol has the
  /// @dynamicCallable attribute on it.
  bool hasDynamicCallableAttribute();

  /// Retrieve the superclass of this type.
  ///
  /// \param useArchetypes Whether to use context archetypes for outer generic
  /// parameters if the class is nested inside a generic function.
  ///
  /// \returns The superclass of this type, or a null type if it has no
  ///          superclass.
  Type getSuperclass(bool useArchetypes = true);

  /// Retrieve the root class of this type by repeatedly retrieving the
  /// superclass.
  ///
  /// \param useArchetypes Whether to use context archetypes for outer generic
  /// parameters if the class is nested inside a generic function.
  ///
  /// \returns The base class of this type, or this type itself if it has no
  ///          superclasses.
  Type getRootClass(bool useArchetypes = true);

  /// True if this type is the exact superclass of another type.
  ///
  /// \param ty       The potential subclass.
  ///
  /// \returns True if this type is \c ty or a superclass of \c ty.
  ///
  /// If this type is a bound generic class \c Foo<T>, the method only
  /// returns true if the generic parameters of \c ty exactly match the
  /// superclass of \c ty. For instance, if \c ty is a
  /// class DerivedClass: Base<Int>, then \c Base<T> (where T is an archetype)
  /// will return false. `isBindableToSuperclassOf` should be used
  /// for queries that care whether a generic class type can be substituted into
  /// a type's subclass.
  bool isExactSuperclassOf(Type ty);

  /// Get the substituted base class type, starting from a base class
  /// declaration and a substituted derived class type.
  ///
  /// For example, given the following declarations:
  ///
  /// class A<T, U> {}
  /// class B<V> : A<Int, V> {}
  /// class C<X, Y> : B<Y> {}
  ///
  /// Calling `C<String, NSObject>`->getSuperclassForDecl(`A`) will return
  /// `A<Int, NSObject>`.
  ///
  /// \param useArchetypes Whether to use context archetypes for outer generic
  /// parameters if the class is nested inside a generic function.
  Type getSuperclassForDecl(const ClassDecl *classDecl,
                            bool useArchetypes = true);

  /// True if this type is the superclass of another type, or a generic
  /// type that could be bound to the superclass.
  ///
  /// \param ty       The potential subclass.
  ///
  /// \returns True if this type is \c ty, a superclass of \c ty, or an
  ///          archetype-parameterized type that can be bound to a superclass
  ///          of \c ty.
  bool isBindableToSuperclassOf(Type ty);

  /// True if this type contains archetypes that could be substituted with
  /// concrete types to form the argument type.
  bool isBindableTo(Type ty);
  
  /// Visit this type and the argument type in parallel, invoking the callback
  /// function with each archetype-to-substituted-type binding. The callback
  /// may return a new type to substitute into the result type, or return
  /// CanType() to error out of the operation. Each invocation of the callback
  /// receives three arguments:
  /// - The `orig` archetype from a position in `this` type.
  /// - The `subst` type in the same structural position of `ty` that is trying to be bound
  ///  to `orig`.
  /// - The `upperBound` archetype, which if set, indicates the minimum set of constraints
  ///  that any type substituted in this structural position must conform to. May be null,
  ///  indicating an unconstrained context.
  /// - If `upperBound` is set, then the `substConformances` array will contain the
  ///  protocol conformances for `subst` to each of the protocol requirements
  ///  on `upperBound` in `getConformsTo` order.
  ///
  /// Returns the substituted type, or a null CanType() if this type
  /// is not bindable to the substituted type, or the callback returns
  /// CanType().
  CanType substituteBindingsTo(Type ty,
         llvm::function_ref<CanType(ArchetypeType *orig,
                CanType subst,
                ArchetypeType *upperBound,
                ArrayRef<ProtocolConformanceRef> substConformances)> substFn);

  /// Determines whether this type is similar to \p other as defined by
  /// \p matchOptions.
  bool matches(Type other, TypeMatchOptions matchOptions);

  bool matchesParameter(Type other, TypeMatchOptions matchMode);

  /// Determines whether this function type is similar to \p
  /// other as defined by \p matchOptions and the callback \p
  /// paramsAndResultMatch which determines in a client-specific way
  /// whether the parameters and result of the types match.
  bool matchesFunctionType(Type other, TypeMatchOptions matchOptions,
                           llvm::function_ref<bool()> paramsAndResultMatch);

  /// Determines whether this type has a retainable pointer
  /// representation, i.e. whether it is representable as a single,
  /// possibly nil pointer that can be unknown-retained and
  /// unknown-released.
  bool hasRetainablePointerRepresentation();

  /// Given that this type is a reference type, which kind of reference
  /// counting does it use?
  ReferenceCounting getReferenceCounting();

  /// Determines whether this type has a bridgeable object
  /// representation, i.e., whether it is always represented as a single
  /// (non-nil) pointer that can be unknown-retained and
  /// unknown-released.
  ///
  /// This predicate covers all types that can be placed into an
  /// AnyObject without ever requiring a representation change. Note that this
  /// excludes ObjC class metatypes, which may need to be wrapped or unwrapped
  /// when converting from native representation to AnyObject representation.
  bool isBridgeableObjectType();

  /// Determine whether this type is a potentially-bridged value type.
  ///
  /// This predicate doesn't guarantee that the type is bridged, but rather is
  /// a quick way to check whether the type is a value type that could
  /// conceivably be bridged to an Objective-C class type.
  bool isPotentiallyBridgedValueType();

  /// If this is a nominal type or a bound generic nominal type,
  /// returns the (possibly generic) nominal type declaration.
  NominalTypeDecl *getNominalOrBoundGenericNominal();

  /// If this is a nominal type, bound generic nominal type, or
  /// unbound generic nominal type, return the (possibly generic) nominal type
  /// declaration.
  NominalTypeDecl *getAnyNominal();

  /// Determine whether the given type is representable in the given
  /// foreign language.
  std::pair<ForeignRepresentableKind, ProtocolConformance *>
  getForeignRepresentableIn(ForeignLanguage language, const DeclContext *dc);

  /// Determines whether the given Swift type is representable within
  /// the given foreign language.
  ///
  /// A given Swift type is representable in the given foreign
  /// language if the Swift type can be used from source code written
  /// in that language.
  bool isRepresentableIn(ForeignLanguage language, const DeclContext *dc);

  /// Determines whether the type is trivially representable within
  /// the foreign language, meaning that it is both representable in
  /// that language and that the runtime representations are
  /// equivalent.
  bool isTriviallyRepresentableIn(ForeignLanguage language,
                                  const DeclContext *dc);

  /// Given that this is a nominal type or bound generic nominal
  /// type, return its parent type; this will be a null type if the type
  /// is not a nested type.
  Type getNominalParent();

  /// If this is a GenericType, bound generic nominal type, or
  /// unbound generic nominal type, return the (possibly generic) nominal type
  /// declaration.
  GenericTypeDecl *getAnyGeneric();

  /// removeArgumentLabels -  Retrieve a version of this type with all
  /// argument labels removed.
  Type removeArgumentLabels(unsigned numArgumentLabels);

  /// Retrieve the type without any parentheses around it.
  Type getWithoutParens();

  /// Replace the base type of the result type of the given function
  /// type with a new result type, as per a DynamicSelf or other
  /// covariant return transformation.  The optionality of the
  /// existing result will be preserved.
  ///
  /// \param newResultType The new result type.
  ///
  /// \param uncurryLevel The number of uncurry levels to apply before
  /// replacing the type. With uncurry level == 0, this simply
  /// replaces the current type with the new result type.
  Type replaceCovariantResultType(Type newResultType,
                                  unsigned uncurryLevel);

  /// Returns a new function type exactly like this one but with the self
  /// parameter replaced. Only makes sense for function members of types.
  Type replaceSelfParameterType(Type newSelf);

  /// getRValueType - For an @lvalue type, retrieves the underlying object type.
  /// Otherwise, returns the type itself.
  Type getRValueType();

  /// getInOutObjectType - For an inout type, retrieves the underlying object
  /// type.  Otherwise, returns the type itself.
  Type getInOutObjectType();

  /// getWithoutSpecifierType - For a non-materializable type
  /// e.g. @lvalue or inout, retrieves the underlying object type.
  /// Otherwise, returns the type itself.
  Type getWithoutSpecifierType();

  /// getMetatypeInstanceType - Looks through metatypes.
  Type getMetatypeInstanceType();

  /// For a ReferenceStorageType like @unowned, this returns the referent.
  /// Otherwise, it returns the type itself.
  Type getReferenceStorageReferent();

  /// Determine the set of substitutions that should be applied to a
  /// type spelled within the given DeclContext to treat it as a
  /// member of this type.
  ///
  /// For example, given:
  /// \code
  /// struct X<T, U> { }
  /// extension X {
  ///   typealias SomeArray = [T]
  /// }
  /// \endcode
  ///
  /// Asking for the member substitutions of \c X<Int,String> within
  /// the context of the extension above will produce substitutions T
  /// -> Int and U -> String suitable for mapping the type of
  /// \c SomeArray.
  ///
  /// \param genericEnv If non-null and the type is nested inside of a
  /// generic function, generic parameters of the outer context are
  /// mapped to context archetypes of this generic environment.
  SubstitutionMap getContextSubstitutionMap(ModuleDecl *module,
                                            const DeclContext *dc,
                                            GenericEnvironment *genericEnv=nullptr);

  /// Deprecated version of the above.
  TypeSubstitutionMap getContextSubstitutions(const DeclContext *dc,
                                              GenericEnvironment *genericEnv=nullptr);

  /// Get the substitutions to apply to the type of the given member as seen
  /// from this base type.
  ///
  /// \param genericEnv If non-null, generic parameters of the member are
  /// mapped to context archetypes of this generic environment.
  SubstitutionMap getMemberSubstitutionMap(ModuleDecl *module,
                                           const ValueDecl *member,
                                           GenericEnvironment *genericEnv=nullptr);

  /// Deprecated version of the above.
  TypeSubstitutionMap getMemberSubstitutions(const ValueDecl *member,
                                             GenericEnvironment *genericEnv=nullptr);

  /// Retrieve the type of the given property as seen through the given base
  /// type, substituting generic arguments where necessary. This is the same as
  /// the more general overload of \c TypeBase::getTypeOfMember, but defaults to
  /// the property's interface type for the \c memberType.
  ///
  /// \param module The module in which the substitution occurs.
  ///
  /// \param member The property whose type we are substituting.
  ///
  /// \returns The resulting property type.
  Type getTypeOfMember(ModuleDecl *module, const VarDecl *member);

  /// Retrieve the type of the given member as seen through the given base
  /// type, substituting generic arguments where necessary.
  ///
  /// This routine allows one to take a concrete type (the "self" type) and
  /// and a member of that type (or one of its superclasses), then determine
  /// what type an access to that member through the base type will have.
  /// For example, given:
  ///
  /// \code
  /// class Vector<T> {
  ///   func add(value : T) { }
  /// }
  /// \endcode
  ///
  /// Given the type \c Vector<Int>, the member \c add, and its method interface
  /// type (value: T) -> Void the resulting type will be (value: Int) -> Void.
  ///
  /// \param module The module in which the substitution occurs.
  ///
  /// \param member The member whose type we are substituting.
  ///
  /// \param memberType The type of the member in which generic parameters will
  /// be replaced by the generic arguments provided by the base type. Note this
  /// must not be a GenericFunctionType. For a method, either strip the self
  /// parameter and generic signature using e.g \c getMethodInterfaceType, or
  /// use \c substGenericArgs if you want to substitute types for any of the
  /// method's generic parameters.
  ///
  /// \returns The resulting member type.
  Type getTypeOfMember(ModuleDecl *module, const ValueDecl *member,
                       Type memberType);

  /// Get the type of a superclass member as seen from the subclass,
  /// substituting generic parameters, dynamic Self return, and the
  /// 'self' argument type as appropriate.
  Type adjustSuperclassMemberDeclType(const ValueDecl *baseDecl,
                                      const ValueDecl *derivedDecl,
                                      Type memberType);

  /// If this type is T, return Optional<T>.
  Type wrapInOptionalType() const;

  /// Return T if this type is Optional<T>; otherwise, return the null type.
  Type getOptionalObjectType();

  // Return type underlying type of a swift_newtype annotated imported struct;
  // otherwise, return the null type.
  Type getSwiftNewtypeUnderlyingType();

  /// Return the type T after looking through at most one optional type.
  Type lookThroughSingleOptionalType();

  /// Return the type T after looking through all of the optional
  /// types.
  Type lookThroughAllOptionalTypes();

  /// Return the type T after looking through all of the optional
  /// types.
  Type lookThroughAllOptionalTypes(SmallVectorImpl<Type> &optionals);

  /// Return the number of optionals this type is wrapped with.
  unsigned int getOptionalityDepth();

  /// Remove concurrency-related types and constraints from the given
  /// type
  ///
  /// \param recurse Whether to recurse into function types.
  ///
  /// \param dropGlobalActor Whether to drop a global actor from a function
  /// type.
  Type stripConcurrency(bool recurse, bool dropGlobalActor);

  /// Whether this is the AnyObject type.
  bool isAnyObject();

  /// Return true if this type is potentially an AnyObject existential after
  /// substitution.
  bool isPotentiallyAnyObject();

  /// Whether this is an existential composition containing
  /// Error.
  bool isExistentialWithError();

  /// Returns the reduced shape of the type, which represents an equivalence
  /// class for the same-shape generic requirement:
  ///
  /// - The shape of a scalar type is always the empty tuple type ().
  /// - The shape of a pack archetype is computed from the generic signature
  ///   using same-shape requirements.
  /// - The shape of a pack type is computed recursively from its elements.
  ///
  /// Two types satisfy a same-shape requirement if their reduced shapes are
  /// equal as canonical types.
  CanType getReducedShape();

  SWIFT_DEBUG_DUMP;
  void dump(raw_ostream &os, unsigned indent = 0) const;

  SWIFT_DEBUG_DUMPER(dumpPrint());
  void print(raw_ostream &OS,
             const PrintOptions &PO = PrintOptions()) const;
  void print(ASTPrinter &Printer, const PrintOptions &PO) const;

  /// Can this type be written in source at all?
  ///
  /// If not, it shouldn't be shown in fix-its, for instance. The primary
  /// example is opaque result types, which are written `some P` at the point
  /// of definition but cannot be uttered anywhere else.
  bool hasTypeRepr() const;

  /// Does this type have grammatically simple syntax?
  bool hasSimpleTypeRepr() const;

  /// Return the name of the type as a string, for use in diagnostics only.
  std::string getString(const PrintOptions &PO = PrintOptions()) const;

  /// Return the name of the type, adding parens in cases where
  /// appending or prepending text to the result would cause that text
  /// to be appended to only a portion of the returned type. For
  /// example for a function type "Int -> Float", adding text after
  /// the type would make it appear that it's appended to "Float" as
  /// opposed to the entire type.
  std::string
  getStringAsComponent(const PrintOptions &PO = PrintOptions()) const;

  /// Return whether this type is or can be substituted for a bridgeable
  /// object type.
  TypeTraitResult canBeClass();

  Type replaceSubstitutedSILFunctionTypesWithUnsubstituted(SILModule &M) const; // in SILType.cpp
  
  /// Return the tangent space of the given type, if it exists. Otherwise,
  /// return `None`.
  Optional<TangentSpace>
  getAutoDiffTangentSpace(LookupConformanceFn lookupConformance);
};

/// AnyGenericType - This abstract class helps types ensure that fields
/// exist at the same offset in memory to improve code generation of the
/// compiler itself.
class AnyGenericType : public TypeBase {
  friend class NominalOrBoundGenericNominalType;

  /// TheDecl - This is the TypeDecl which declares the given type. It
  /// specifies the name and other useful information about this type.
  union {
    GenericTypeDecl *GenDecl;
    NominalTypeDecl *NomDecl;
  };

  /// The type of the parent, in which this type is nested.
  Type Parent;

  template <typename... Args>
  AnyGenericType(NominalTypeDecl *TheDecl, Type Parent, Args &&...args)
    : TypeBase(std::forward<Args>(args)...), NomDecl(TheDecl), Parent(Parent) {}

protected:
  template <typename... Args>
  AnyGenericType(GenericTypeDecl *TheDecl, Type Parent, Args &&...args)
    : TypeBase(std::forward<Args>(args)...), GenDecl(TheDecl), Parent(Parent) {}

public:

  /// Returns the declaration that declares this type.
  GenericTypeDecl *getDecl() const { return GenDecl; }

  /// Returns the type of the parent of this type. This will
  /// be null for top-level types or local types, and for non-generic types
  /// will simply be the same as the declared type of the declaration context
  /// of TheDecl. For types nested within generic types, however, this will
  /// involve \c BoundGenericType nodes that provide context for the nested
  /// type, e.g., the type Dictionary<String, Int>.ItemRange would be
  /// represented as a NominalType with Dictionary<String, Int> as its parent
  /// type.
  Type getParent() const { return Parent; }

  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() >= TypeKind::First_AnyGenericType &&
           T->getKind() <= TypeKind::Last_AnyGenericType;
  }
};
BEGIN_CAN_TYPE_WRAPPER(AnyGenericType, Type)
  PROXY_CAN_TYPE_SIMPLE_GETTER(getParent)
END_CAN_TYPE_WRAPPER(AnyGenericType, Type)

/// NominalOrBoundGenericNominal - This abstract class helps types ensure that
/// fields exist at the same offset in memory to improve code generation of the
/// compiler itself.
class NominalOrBoundGenericNominalType : public AnyGenericType {
public:
  template <typename... Args>
  NominalOrBoundGenericNominalType(Args &&...args)
    : AnyGenericType(std::forward<Args>(args)...) {}

  /// Returns the declaration that declares this type.
  NominalTypeDecl *getDecl() const { return NomDecl; }

  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() >= TypeKind::First_NominalOrBoundGenericNominalType &&
           T->getKind() <= TypeKind::Last_NominalOrBoundGenericNominalType;
  }
};
DEFINE_EMPTY_CAN_TYPE_WRAPPER(NominalOrBoundGenericNominalType, AnyGenericType)

/// ErrorType - Represents the type of an erroneously constructed declaration,
/// expression, or type. When creating ErrorTypes, an associated error
/// diagnostic should always be emitted. That way when later stages of
/// compilation encounter an ErrorType installed by earlier phases they do not
/// have to emit further diagnostics to abort compilation.
class ErrorType final : public TypeBase {
  friend class ASTContext;
  // The Error type is always canonical.
  ErrorType(ASTContext &C, Type originalType,
            RecursiveTypeProperties properties)
      : TypeBase(TypeKind::Error, &C, properties) {
    assert(properties.hasError());
    if (originalType) {
      Bits.ErrorType.HasOriginalType = true;
      *reinterpret_cast<Type *>(this + 1) = originalType;
    } else {
      Bits.ErrorType.HasOriginalType = false;
    }
  }

public:
  static Type get(const ASTContext &C);

  /// Produce an error type which records the original type we were trying to
  /// substitute when we ran into a problem.
  static Type get(Type originalType);

  /// Retrieve the original type that this error type replaces, or none if
  /// there is no such type.
  Type getOriginalType() const {
    if (Bits.ErrorType.HasOriginalType)
      return *reinterpret_cast<const Type *>(this + 1);

    return Type();
  }

  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::Error;
  }
};
DEFINE_EMPTY_CAN_TYPE_WRAPPER(ErrorType, Type)

/// UnresolvedType - This represents a type variable that cannot be resolved to
/// a concrete type because the expression is ambiguous.  This is produced when
/// parsing expressions and producing diagnostics.  Any instance of this should
/// cause the entire expression to be ambiguously typed.
class UnresolvedType : public TypeBase {
  friend class ASTContext;
  // The Unresolved type is always canonical.
  UnresolvedType(ASTContext &C)
    : TypeBase(TypeKind::Unresolved, &C,
       RecursiveTypeProperties(RecursiveTypeProperties::HasUnresolvedType)) { }
public:
  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::Unresolved;
  }
};
DEFINE_EMPTY_CAN_TYPE_WRAPPER(UnresolvedType, Type)

  
/// BuiltinType - An abstract class for all the builtin types.
class BuiltinType : public TypeBase {
protected:
  BuiltinType(TypeKind kind, const ASTContext &canTypeCtx)
  : TypeBase(kind, &canTypeCtx, RecursiveTypeProperties()) {}
public:
  static bool classof(const TypeBase *T) {
    return T->getKind() >= TypeKind::First_BuiltinType &&
           T->getKind() <= TypeKind::Last_BuiltinType;
  }

  /// Return the "canonical" name for this builtin. E.x.:
  ///
  ///   BuiltinRawPointerType -> BUILTIN_TYPE_NAME_RAWPOINTER ->
  ///   Builtin.RawPointer.
  ///
  /// If \p prependBuiltinNamespace is set to true, "Builtin." is left as a
  /// prefix on the name. This is the default behavior. If the user asks, we
  /// strip off the builtin prefix.
  StringRef getTypeName(SmallVectorImpl<char> &result,
                        bool prependBuiltinNamespace = true) const;

  BuiltinTypeKind getBuiltinTypeKind() const;
};
DEFINE_EMPTY_CAN_TYPE_WRAPPER(BuiltinType, Type)

/// BuiltinRawPointerType - The builtin raw (and dangling) pointer type.  This
/// pointer is completely unmanaged and is equivalent to i8* in LLVM IR.
class BuiltinRawPointerType : public BuiltinType {
  friend class ASTContext;
  BuiltinRawPointerType(const ASTContext &C)
    : BuiltinType(TypeKind::BuiltinRawPointer, C) {}
public:
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::BuiltinRawPointer;
  }
};
DEFINE_EMPTY_CAN_TYPE_WRAPPER(BuiltinRawPointerType, BuiltinType)

/// BuiltinRawContinuationType - The builtin raw unsafe continuation type.
/// In C, this is a non-null AsyncTask*.  This pointer is completely
/// unmanaged (the unresumed task is self-owning), but has more spare bits
/// than Builtin.RawPointer.
class BuiltinRawUnsafeContinuationType : public BuiltinType {
  friend class ASTContext;
  BuiltinRawUnsafeContinuationType(const ASTContext &C)
    : BuiltinType(TypeKind::BuiltinRawUnsafeContinuation, C) {}
public:
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::BuiltinRawUnsafeContinuation;
  }
};
DEFINE_EMPTY_CAN_TYPE_WRAPPER(BuiltinRawUnsafeContinuationType, BuiltinType)

/// BuiltinExecutorType - The builtin executor-ref type.  In C, this
/// is the ExecutorRef struct type.
class BuiltinExecutorType : public BuiltinType {
  friend class ASTContext;
  BuiltinExecutorType(const ASTContext &C)
    : BuiltinType(TypeKind::BuiltinExecutor, C) {}
public:
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::BuiltinExecutor;
  }
};
DEFINE_EMPTY_CAN_TYPE_WRAPPER(BuiltinExecutorType, BuiltinType)

/// BuiltinJobType - The builtin job type.  In C, this is a
/// non-null Job*.  This pointer is completely unmanaged (the unscheduled
/// job is self-owning), but has more spare bits than Builtin.RawPointer.
class BuiltinJobType : public BuiltinType {
  friend class ASTContext;
  BuiltinJobType(const ASTContext &C)
    : BuiltinType(TypeKind::BuiltinJob, C) {}
public:
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::BuiltinJob;
  }
};
DEFINE_EMPTY_CAN_TYPE_WRAPPER(BuiltinJobType, BuiltinType)

/// BuiltinDefaultActorStorageType - The type of the stored property
/// that's added implicitly to default actors.  No C equivalent because
/// the C types all include a heap-object header.  Similarly, this type
/// generally does not appear in the AST/SIL around default actors;
/// it's purely a convenience in IRGen.
class BuiltinDefaultActorStorageType : public BuiltinType {
  friend class ASTContext;
  BuiltinDefaultActorStorageType(const ASTContext &C)
    : BuiltinType(TypeKind::BuiltinDefaultActorStorage, C) {}
public:
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::BuiltinDefaultActorStorage;
  }
};
DEFINE_EMPTY_CAN_TYPE_WRAPPER(BuiltinDefaultActorStorageType, BuiltinType)

/// BuiltinNonDefaultDistributedActorStorageType - The type of the stored property
/// that's added implicitly to distributed actors.  No C equivalent because
/// the C types all include a heap-object header.  Similarly, this type
/// generally does not appear in the AST/SIL around default actors;
/// it's purely a convenience in IRGen.
class BuiltinNonDefaultDistributedActorStorageType : public BuiltinType {
  friend class ASTContext;
  BuiltinNonDefaultDistributedActorStorageType(const ASTContext &C)
    : BuiltinType(TypeKind::BuiltinNonDefaultDistributedActorStorage, C) {}
public:
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::BuiltinNonDefaultDistributedActorStorage;
  }
};
DEFINE_EMPTY_CAN_TYPE_WRAPPER(BuiltinNonDefaultDistributedActorStorageType, BuiltinType)

/// BuiltinPackIndexType - The type of an (untyped) index into a pack
/// in SIL.  Essentially a UInt32 with some structural restrictions
/// about how it can be produced that ensures SIL maintains well-typed
/// use-def relationships around packs.
class BuiltinPackIndexType : public BuiltinType {
  friend class ASTContext;
  BuiltinPackIndexType(const ASTContext &C)
    : BuiltinType(TypeKind::BuiltinPackIndex, C) {}
public:
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::BuiltinPackIndex;
  }
};
DEFINE_EMPTY_CAN_TYPE_WRAPPER(BuiltinPackIndexType, BuiltinType)

/// BuiltinNativeObjectType - The builtin opaque object-pointer type.
/// Useful for keeping an object alive when it is otherwise being
/// manipulated via an unsafe pointer type.
class BuiltinNativeObjectType : public BuiltinType {
  friend class ASTContext;
  BuiltinNativeObjectType(const ASTContext &C)
    : BuiltinType(TypeKind::BuiltinNativeObject, C) {}
public:
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::BuiltinNativeObject;
  }
};
DEFINE_EMPTY_CAN_TYPE_WRAPPER(BuiltinNativeObjectType, BuiltinType)

/// A type that contains an owning reference to a heap object packed with
/// additional bits. The type uses a bit to discriminate native Swift objects
/// from Objective-C object pointers or tagged pointers.
class BuiltinBridgeObjectType : public BuiltinType {
  friend class ASTContext;
  BuiltinBridgeObjectType(const ASTContext &C)
    : BuiltinType(TypeKind::BuiltinBridgeObject, C) {}
public:
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::BuiltinBridgeObject;
  }
};
DEFINE_EMPTY_CAN_TYPE_WRAPPER(BuiltinBridgeObjectType, BuiltinType)

/// BuiltinUnsafeValueBufferType - The builtin opaque fixed-size value
/// buffer type, into which storage for an arbitrary value can be
/// allocated using Builtin.allocateValueBuffer.
///
/// This type is unsafe because it does not permit ordinary value
/// operations.  It is essentially an Any without any type
/// information.  It should only be used in narrow circumstances in
/// carefully-written SIL.
class BuiltinUnsafeValueBufferType : public BuiltinType {
  friend class ASTContext;
  BuiltinUnsafeValueBufferType(const ASTContext &C)
    : BuiltinType(TypeKind::BuiltinUnsafeValueBuffer, C) {}
public:
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::BuiltinUnsafeValueBuffer;
  }
};
DEFINE_EMPTY_CAN_TYPE_WRAPPER(BuiltinUnsafeValueBufferType, BuiltinType)

/// A builtin vector type.
class BuiltinVectorType : public BuiltinType, public llvm::FoldingSetNode {
  Type elementType;
  unsigned numElements;

  friend class ASTContext;

  BuiltinVectorType(const ASTContext &context, Type elementType,
                    unsigned numElements)
    : BuiltinType(TypeKind::BuiltinVector, context),
      elementType(elementType), numElements(numElements) { }

public:
  static BuiltinVectorType *get(const ASTContext &context, Type elementType,
                                unsigned numElements);

  /// Retrieve the type of this vector's elements.
  Type getElementType() const { return elementType; }

  /// Retrieve the number of elements in this vector.
  unsigned getNumElements() const { return numElements; }

  void Profile(llvm::FoldingSetNodeID &ID) {
    Profile(ID, getElementType(), getNumElements());
  }
  static void Profile(llvm::FoldingSetNodeID &ID, Type elementType,
                      unsigned numElements) {
    ID.AddPointer(elementType.getPointer());
    ID.AddInteger(numElements);
  }

  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::BuiltinVector;
  }
};
BEGIN_CAN_TYPE_WRAPPER(BuiltinVectorType, BuiltinType)
  PROXY_CAN_TYPE_SIMPLE_GETTER(getElementType)
END_CAN_TYPE_WRAPPER(BuiltinVectorType, BuiltinType)

/// Size descriptor for a builtin integer type. This is either a fixed bit
/// width or an abstract target-dependent value such as "size of a pointer".
class BuiltinIntegerWidth {
  /// Tag values for abstract integer sizes.
  enum : unsigned {
    /// Inhabitants stolen for use as DenseMap special values.
    DenseMapEmpty = ~0U,
    DenseMapTombstone = ~1U,

    /// An arbitrary-precision integer.
    ArbitraryWidth = ~2U,

    /// The size of a pointer on the target system.
    PointerWidth = ~3U,
    
    Least_SpecialValue = ~3U,
  };
  
  unsigned RawValue;
  
  friend struct llvm::DenseMapInfo<swift::BuiltinIntegerWidth>;
  
  /// Private constructor from a raw symbolic value.
  explicit BuiltinIntegerWidth(unsigned RawValue) : RawValue(RawValue) {}
public:
  BuiltinIntegerWidth() : RawValue(0) {}
  
  static BuiltinIntegerWidth fixed(unsigned bitWidth) {
    assert(bitWidth < Least_SpecialValue && "invalid bit width");
    return BuiltinIntegerWidth(bitWidth);
  }
  
  static BuiltinIntegerWidth pointer() {
    return BuiltinIntegerWidth(PointerWidth);
  }

  static BuiltinIntegerWidth arbitrary() {
    return BuiltinIntegerWidth(ArbitraryWidth);
  }
  
  /// Is this a fixed width?
  bool isFixedWidth() const { return RawValue < Least_SpecialValue; }

  /// Get the fixed width value. Fails if the width is abstract.
  unsigned getFixedWidth() const {
    assert(isFixedWidth() && "not fixed-width");
    return RawValue;
  }
  
  /// Is this the abstract target pointer width?
  bool isPointerWidth() const { return RawValue == PointerWidth; }

  /// Is this the abstract arbitrary-width value?
  bool isArbitraryWidth() const { return RawValue == ArbitraryWidth; }
  
  /// Get the least supported value for the width.
  ///
  /// FIXME: This should be build-configuration-dependent.
  unsigned getLeastWidth() const {
    if (isFixedWidth())
      return getFixedWidth();
    if (isPointerWidth())
      return 32;
    if (isArbitraryWidth())
      return 1;
    llvm_unreachable("impossible width value");
  }
  
  /// Get the greatest supported value for the width.
  ///
  /// FIXME: This should be build-configuration-dependent.
  unsigned getGreatestWidth() const {
    if (isFixedWidth())
      return getFixedWidth();
    if (isPointerWidth())
      return 64;
    if (isArbitraryWidth())
      return ~0U;
    llvm_unreachable("impossible width value");
  }

  /// Parse a value of this bit-width.
  ///
  /// If the radix is 0, it is autosensed.
  APInt parse(StringRef text, unsigned radix, bool negate,
              bool *hadError = nullptr) const;
  
  friend bool operator==(BuiltinIntegerWidth a, BuiltinIntegerWidth b) {
    return a.RawValue == b.RawValue;
  }
  friend bool operator!=(BuiltinIntegerWidth a, BuiltinIntegerWidth b) {
    return a.RawValue != b.RawValue;
  }
};

/// An abstract base class for the two integer types.
class AnyBuiltinIntegerType : public BuiltinType {
protected:
  AnyBuiltinIntegerType(TypeKind kind, const ASTContext &C)
    : BuiltinType(kind, C) {}

public:
  static bool classof(const TypeBase *T) {
    return T->getKind() >= TypeKind::First_AnyBuiltinIntegerType &&
           T->getKind() <= TypeKind::Last_AnyBuiltinIntegerType;
  }

  BuiltinIntegerWidth getWidth() const; // defined inline below
};
DEFINE_EMPTY_CAN_TYPE_WRAPPER(AnyBuiltinIntegerType, BuiltinType)

/// The builtin integer types.  These directly correspond
/// to LLVM IR integer types.  They lack signedness and have an arbitrary
/// bitwidth.
class BuiltinIntegerType : public AnyBuiltinIntegerType {
  friend class ASTContext;
private:
  BuiltinIntegerWidth Width;
  BuiltinIntegerType(BuiltinIntegerWidth BitWidth, const ASTContext &C)
    : AnyBuiltinIntegerType(TypeKind::BuiltinInteger, C), Width(BitWidth) {}
  
public:
  /// Get a builtin integer type.
  static BuiltinIntegerType *get(BuiltinIntegerWidth BitWidth,
                                 const ASTContext &C);
  
  /// Get a builtin integer type of fixed width.
  static BuiltinIntegerType *get(unsigned BitWidth, const ASTContext &C) {
    return get(BuiltinIntegerWidth::fixed(BitWidth), C);
  }
  
  /// Get the target-pointer-width builtin integer type.
  static BuiltinIntegerType *getWordType(const ASTContext &C) {
    return get(BuiltinIntegerWidth::pointer(), C);
  }
  
  /// Return the bit width of the integer.  Always returns a non-arbitrary
  /// width.
  BuiltinIntegerWidth getWidth() const {
    return Width;
  }
  
  /// Is the integer fixed-width?
  bool isFixedWidth() const {
    return Width.isFixedWidth();
  }
  
  /// Is the integer fixed-width with the given width?
  bool isFixedWidth(unsigned width) const {
    return Width.isFixedWidth() && Width.getFixedWidth() == width;
  }
  
  /// Get the fixed integer width. Fails if the integer has abstract width.
  unsigned getFixedWidth() const {
    return Width.getFixedWidth();
  }
  
  /// Return the least supported width of the integer.
  ///
  /// FIXME: This should be build-configuration-dependent.
  unsigned getLeastWidth() const {
    return Width.getLeastWidth();
  }
  
  /// Return the greatest supported width of the integer.
  ///
  /// FIXME: This should be build-configuration-dependent.
  unsigned getGreatestWidth() const {
    return Width.getGreatestWidth();
  }

  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::BuiltinInteger;
  }
};
DEFINE_EMPTY_CAN_TYPE_WRAPPER(BuiltinIntegerType, AnyBuiltinIntegerType)

/// BuiltinIntegerLiteralType - The builtin arbitrary-precision integer type.
/// Useful for constructing integer literals.
class BuiltinIntegerLiteralType : public AnyBuiltinIntegerType {
  friend class ASTContext;
  BuiltinIntegerLiteralType(const ASTContext &C)
    : AnyBuiltinIntegerType(TypeKind::BuiltinIntegerLiteral, C) {}
public:
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::BuiltinIntegerLiteral;
  }

  BuiltinIntegerWidth getWidth() const = delete;
};
DEFINE_EMPTY_CAN_TYPE_WRAPPER(BuiltinIntegerLiteralType, AnyBuiltinIntegerType)

inline BuiltinIntegerWidth AnyBuiltinIntegerType::getWidth() const {
  if (auto intTy = dyn_cast<BuiltinIntegerType>(this)) {
    return intTy->getWidth();
  } else {
    return BuiltinIntegerWidth::arbitrary();
  }
}

class BuiltinFloatType : public BuiltinType {
  friend class ASTContext;
public:
  enum FPKind {
    IEEE16, IEEE32, IEEE64, IEEE80, IEEE128, /// IEEE floating point types.
    PPC128   /// PowerPC "double double" type.
  };
private:
  FPKind Kind;
  
  BuiltinFloatType(FPKind Kind, const ASTContext &C)
    : BuiltinType(TypeKind::BuiltinFloat, C), Kind(Kind) {}
public:
  
  /// getFPKind - Get the 
  FPKind getFPKind() const {
    return Kind;
  }

  const llvm::fltSemantics &getAPFloatSemantics() const;

  unsigned getBitWidth() const {
    switch (Kind) {
    case IEEE16: return 16;
    case IEEE32: return 32;
    case IEEE64: return 64;
    case IEEE80: return 80;
    case IEEE128:
    case PPC128: return 128;
    }
    llvm_unreachable("bad FPKind");
  }

  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::BuiltinFloat;
  }
};
DEFINE_EMPTY_CAN_TYPE_WRAPPER(BuiltinFloatType, BuiltinType)
  
/// An abstract type for all sugared types to make getDesugaredType() fast by
/// sharing field offsets and logic for the fast path.
class SugarType : public TypeBase {
  // The state of this union is known via Bits.SugarType.HasCachedType so that
  // we can avoid masking the pointer on the fast path.
  union {
    TypeBase *UnderlyingType;
    const ASTContext *Context;
  };

protected:
  // Sugar types are never canonical.
  SugarType(TypeKind K, const ASTContext *ctx,
            RecursiveTypeProperties properties)
      : TypeBase(K, nullptr, properties), Context(ctx) {
    assert(ctx != nullptr &&
           "Context for SugarType should not be null");
    Bits.SugarType.HasCachedType = false;
  }

  // Sugar types are never canonical.
  SugarType(TypeKind K, Type type, RecursiveTypeProperties properties)
      : TypeBase(K, nullptr, properties), UnderlyingType(type.getPointer()) {
    Bits.SugarType.HasCachedType = true;
  }

public:
  /// Remove one level of top-level sugar from this type.
  Type getSinglyDesugaredTypeSlow();
  TypeBase *getSinglyDesugaredType() const {
    if (LLVM_LIKELY(Bits.SugarType.HasCachedType))
      return UnderlyingType;
    auto Ty = const_cast<SugarType*>(this);
    return Ty->getSinglyDesugaredTypeSlow().getPointer();
  }

  static bool classof(const TypeBase *T) {
    // Workaround: http://llvm.org/PR35906
    if (TypeKind::Last_Type == TypeKind::Last_SugarType)
      return T->getKind() >= TypeKind::First_SugarType;
    return T->getKind() >= TypeKind::First_SugarType &&
           T->getKind() <= TypeKind::Last_SugarType;
  }
};

/// A reference to a type alias that is somehow generic, along with the
/// set of substitutions to apply to make the type concrete.
class TypeAliasType final
  : public SugarType, public llvm::FoldingSetNode,
    llvm::TrailingObjects<TypeAliasType, Type, SubstitutionMap>
{
  TypeAliasDecl *typealias;

  friend class ASTContext;
  friend TrailingObjects;

  TypeAliasType(TypeAliasDecl *typealias, Type parent,
                SubstitutionMap substitutions, Type underlying,
                RecursiveTypeProperties properties);

  size_t numTrailingObjects(OverloadToken<Type>) const {
    return Bits.TypeAliasType.HasParent ? 1 : 0;
  }

  size_t numTrailingObjects(OverloadToken<SubstitutionMap>) const {
    return Bits.TypeAliasType.HasSubstitutionMap ? 1 : 0;
  }

public:
  /// Retrieve the generic signature used for substitutions.
  GenericSignature getGenericSignature() const {
    return getSubstitutionMap().getGenericSignature();
  }

  static TypeAliasType *get(TypeAliasDecl *typealias, Type parent,
                            SubstitutionMap substitutions, Type underlying);

  /// Returns the declaration that declares this type.
  TypeAliasDecl *getDecl() const {
    // Avoid requiring the definition of TypeAliasDecl.
    return typealias;
  }

  /// Retrieve the parent of this type as written, e.g., the part that was
  /// written before ".", if provided.
  Type getParent() const {
    return Bits.TypeAliasType.HasParent ? *getTrailingObjects<Type>()
                                        : Type();
  }

  /// Retrieve the substitution map applied to the declaration's underlying
  /// to produce the described type.
  SubstitutionMap getSubstitutionMap() const {
    if (!Bits.TypeAliasType.HasSubstitutionMap)
      return SubstitutionMap();

    return *getTrailingObjects<SubstitutionMap>();
  }

  /// Get the direct generic arguments, which correspond to the generic
  /// arguments that are directly applied to the typealias declaration
  /// this type references.
  ArrayRef<Type> getDirectGenericArgs() const;

  SmallVector<Type, 2> getExpandedGenericArgs();

  // Support for FoldingSet.
  void Profile(llvm::FoldingSetNodeID &id) const;

  static void Profile(llvm::FoldingSetNodeID &id, TypeAliasDecl *typealias,
                      Type parent, SubstitutionMap substitutions,
                      Type underlying);

  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::TypeAlias;
  }
};

/// The various spellings of ownership modifier that can be used in source.
enum class ParamSpecifier : uint8_t {
  /// No explicit ownership specifier was provided. The parameter will use the
  /// default ownership convention for the declaration.
  Default = 0,

  /// `inout`, indicating exclusive mutable access to the argument for the
  /// duration of a call.
  InOut = 1,

  /// `borrowing`, indicating nonexclusive access to the argument for the
  /// duration of a call.
  Borrowing = 2,
  /// `consuming`, indicating ownership transfer of the argument from caller
  /// to callee.
  Consuming = 3,

  /// `__shared`, a legacy spelling of `borrowing`.
  LegacyShared = 4,
  /// `__owned`, a legacy spelling of `consuming`.
  LegacyOwned = 5,
};

/// Provide parameter type relevant flags, i.e. variadic, autoclosure, and
/// escaping.
class ParameterTypeFlags {
  enum ParameterFlags : uint16_t {
    None         = 0,
    Variadic     = 1 << 0,
    AutoClosure  = 1 << 1,
    NonEphemeral = 1 << 2,
    SpecifierShift = 3,
    Specifier    = 7 << SpecifierShift,
    NoDerivative = 1 << 6,
    Isolated     = 1 << 7,
    CompileTimeConst = 1 << 8,
    NumBits = 9
  };
  OptionSet<ParameterFlags> value;
  static_assert(NumBits <= 8*sizeof(OptionSet<ParameterFlags>), "overflowed");

  ParameterTypeFlags(OptionSet<ParameterFlags, uint16_t> val) : value(val) {}

public:
  ParameterTypeFlags() = default;
  static ParameterTypeFlags fromRaw(uint16_t raw) {
    return ParameterTypeFlags(OptionSet<ParameterFlags>(raw));
  }

  ParameterTypeFlags(bool variadic, bool autoclosure, bool nonEphemeral,
                     ParamSpecifier specifier, bool isolated, bool noDerivative,
                     bool compileTimeConst)
      : value((variadic ? Variadic : 0) | (autoclosure ? AutoClosure : 0) |
              (nonEphemeral ? NonEphemeral : 0) |
              uint8_t(specifier) << SpecifierShift |
              (isolated ? Isolated : 0) |
              (noDerivative ? NoDerivative : 0) |
              (compileTimeConst ? CompileTimeConst : 0)){}

  /// Create one from what's present in the parameter type
  inline static ParameterTypeFlags
  fromParameterType(Type paramTy, bool isVariadic, bool isAutoClosure,
                    bool isNonEphemeral, ParamSpecifier ownership,
                    bool isolated, bool isNoDerivative,
                    bool compileTimeConst);

  bool isNone() const { return !value; }
  bool isVariadic() const { return value.contains(Variadic); }
  bool isAutoClosure() const { return value.contains(AutoClosure); }
  bool isNonEphemeral() const { return value.contains(NonEphemeral); }
  bool isInOut() const { return getValueOwnership() == ValueOwnership::InOut; }
  bool isShared() const { return getValueOwnership() == ValueOwnership::Shared;}
  bool isOwned() const { return getValueOwnership() == ValueOwnership::Owned; }
  bool isIsolated() const { return value.contains(Isolated); }
  bool isCompileTimeConst() const { return value.contains(CompileTimeConst); }
  bool isNoDerivative() const { return value.contains(NoDerivative); }

  /// Get the spelling of the parameter specifier used on the parameter.
  ParamSpecifier getOwnershipSpecifier() const {
    return ParamSpecifier((value.toRaw() & Specifier) >> SpecifierShift);
  }

  ValueOwnership getValueOwnership() const;

  ParameterTypeFlags withVariadic(bool variadic) const {
    return ParameterTypeFlags(variadic ? value | ParameterTypeFlags::Variadic
                                       : value - ParameterTypeFlags::Variadic);
  }

  ParameterTypeFlags withInOut(bool isInout) const {
    return withOwnershipSpecifier(isInout ? ParamSpecifier::InOut
                                          : ParamSpecifier::Default);
  }

  ParameterTypeFlags withCompileTimeConst(bool isConst) const {
    return ParameterTypeFlags(isConst ? value | ParameterTypeFlags::CompileTimeConst
                                      : value - ParameterTypeFlags::CompileTimeConst);
  }
  
  ParameterTypeFlags withShared(bool isShared) const {
    return withOwnershipSpecifier(isShared ? ParamSpecifier::LegacyShared
                                           : ParamSpecifier::Default);
  }

  ParameterTypeFlags withOwned(bool isOwned) const {
    return withOwnershipSpecifier(isOwned ? ParamSpecifier::LegacyOwned
                                          : ParamSpecifier::Default);
  }

  ParameterTypeFlags withOwnershipSpecifier(ParamSpecifier specifier) const {
    return (value - ParameterTypeFlags::Specifier)
            | ParameterFlags(uint8_t(specifier) << SpecifierShift);
  }

  ParameterTypeFlags withAutoClosure(bool isAutoClosure) const {
    return ParameterTypeFlags(isAutoClosure
                                  ? value | ParameterTypeFlags::AutoClosure
                                  : value - ParameterTypeFlags::AutoClosure);
  }

  ParameterTypeFlags withNonEphemeral(bool isNonEphemeral) const {
    return ParameterTypeFlags(isNonEphemeral
                                  ? value | ParameterTypeFlags::NonEphemeral
                                  : value - ParameterTypeFlags::NonEphemeral);
  }

  ParameterTypeFlags withIsolated(bool isolated) const {
    return ParameterTypeFlags(isolated
                                  ? value | ParameterTypeFlags::Isolated
                                  : value - ParameterTypeFlags::Isolated);
  }

  ParameterTypeFlags withNoDerivative(bool noDerivative) const {
    return ParameterTypeFlags(noDerivative
                                  ? value | ParameterTypeFlags::NoDerivative
                                  : value - ParameterTypeFlags::NoDerivative);
  }

  bool operator ==(const ParameterTypeFlags &other) const {
    return value.toRaw() == other.value.toRaw();
  }

  bool operator!=(const ParameterTypeFlags &other) const {
    return value.toRaw() != other.value.toRaw();
  }

  uint16_t toRaw() const { return value.toRaw(); }
};

/// A type that indicates how parameter flags should be handled in an operation
/// that requires the conversion into a type that doesn't support them, such as
/// tuples.
enum class ParameterFlagHandling {
  /// Ignores any parameter flags that may be present, dropping them from the
  /// result. This should only be used in specific cases, including e.g:
  ///
  /// - The flags have already been handled, and unsuitable flags have been
  ///   rejected or asserted to not be present.
  /// - The flags aren't relevant for the particular conversion (e.g for type
  ///   printing or compatibility logic).
  /// - The conversion is only interested in the 'internal argument' of a
  ///   parameter, in which case only the type and label are relevant.
  ///
  /// In all other cases, you ought to verify that unsuitable flags are not
  /// present, or add assertions to that effect.
  IgnoreNonEmpty,

  /// Asserts that no parameter flags are present.
  AssertEmpty
};

class YieldTypeFlags {
  enum YieldFlags : uint8_t {
    None        = 0,
    Specifier   = 7,
    SpecifierShift = 0,

    NumBits = 3
  };
  OptionSet<YieldFlags> value;

  static_assert(NumBits < 8 * sizeof(OptionSet<YieldFlags>), "overflowed");

  YieldTypeFlags(OptionSet<YieldFlags, uint8_t> val) : value(val) {}

public:
  YieldTypeFlags() = default;
  static YieldTypeFlags fromRaw(uint8_t raw) {
    return YieldTypeFlags(OptionSet<YieldFlags>(raw));
  }

  YieldTypeFlags(ParamSpecifier specifier)
      : value(uint8_t(specifier) << SpecifierShift) {}

  bool isInOut() const { return getValueOwnership() == ValueOwnership::InOut; }
  bool isShared() const { return getValueOwnership() == ValueOwnership::Shared;}
  bool isOwned() const { return getValueOwnership() == ValueOwnership::Owned; }

  ParamSpecifier getOwnershipSpecifier() const {
    return ParamSpecifier((value.toRaw() & Specifier) >> SpecifierShift);
  }
  
  ValueOwnership getValueOwnership() const;

  YieldTypeFlags withInOut(bool isInout) const {
    return withOwnershipSpecifier(isInout ? ParamSpecifier::InOut
                                          : ParamSpecifier::Default);
  }
  
  YieldTypeFlags withShared(bool isShared) const {
    return withOwnershipSpecifier(isShared ? ParamSpecifier::LegacyShared
                                           : ParamSpecifier::Default);
  }

  YieldTypeFlags withOwned(bool isOwned) const {
    return withOwnershipSpecifier(isOwned ? ParamSpecifier::LegacyOwned
                                          : ParamSpecifier::Default);
  }

  YieldTypeFlags withOwnershipSpecifier(ParamSpecifier ownership) const {
    return (value - YieldTypeFlags::Specifier)
            | YieldFlags(uint8_t(ownership) << SpecifierShift);
  }

  /// Return these flags interpreted as parameter flags.
  ParameterTypeFlags asParamFlags() const {
    return ParameterTypeFlags(/*variadic*/ false,
                              /*autoclosure*/ false,
                              /*nonEphemeral*/ false, getOwnershipSpecifier(),
                              /*isolated*/ false, /*noDerivative*/ false,
                              /*compileTimeConst*/false);
  }

  bool operator ==(const YieldTypeFlags &other) const {
    return value.toRaw() == other.value.toRaw();
  }

  bool operator!=(const YieldTypeFlags &other) const {
    return value.toRaw() != other.value.toRaw();
  }

  uint8_t toRaw() const { return value.toRaw(); }
};

/// ParenType - A paren type is a type that's been written in parentheses.
class ParenType : public SugarType {
  ParenType(Type UnderlyingType, RecursiveTypeProperties properties);

public:
  static ParenType *get(const ASTContext &C, Type underlying);

  Type getUnderlyingType() const { return getSinglyDesugaredType(); }

  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::Paren;
  }
};

/// TupleTypeElt - This represents a single element of a tuple.
class TupleTypeElt {
  /// An optional name for the field.
  Identifier Name;

  /// This is the type of the field.
  Type ElementType;

  friend class TupleType;
  
public:
  TupleTypeElt() = default;
  TupleTypeElt(Type ty, Identifier name = Identifier());

  bool hasName() const { return !Name.empty(); }
  Identifier getName() const { return Name; }

  Type getType() const { return ElementType; }

  /// Retrieve a copy of this tuple type element with the type replaced.
  TupleTypeElt getWithType(Type T) const;

  /// Retrieve a copy of this tuple type element with the name replaced.
  TupleTypeElt getWithName(Identifier name) const;

  /// Retrieve a copy of this tuple type element with no name
  TupleTypeElt getWithoutName() const { return getWithName(Identifier()); }
};

inline Type getTupleEltType(const TupleTypeElt &elt) {
  return elt.getType();
}
typedef ArrayRefView<TupleTypeElt,Type,getTupleEltType> TupleEltTypeArrayRef;

inline CanType getCanTupleEltType(const TupleTypeElt &elt) {
  return CanType(elt.getType());
}
typedef ArrayRefView<TupleTypeElt,CanType,getCanTupleEltType>
  CanTupleEltTypeArrayRef;

/// TupleType - A tuple is a parenthesized list of types where each name has an
/// optional name.
///
class TupleType final : public TypeBase, public llvm::FoldingSetNode,
    private llvm::TrailingObjects<TupleType, TupleTypeElt> {
  friend TrailingObjects;
  
public:
  /// get - Return the uniqued tuple type with the specified elements.
  ///
  /// This can construct one-element tuple types, which are impossible to
  /// write in the source language. The caller should check for that case
  /// first it a bona fide 'r-value' type is desired.
  static TupleType *get(ArrayRef<TupleTypeElt> Elements, const ASTContext &C);

  /// getEmpty - Return the empty tuple type '()'.
  static CanTypeWrapper<TupleType> getEmpty(const ASTContext &C);

  unsigned getNumElements() const { return Bits.TupleType.Count; }

  /// Returns the number of non-PackExpansionType elements. This is the
  /// minimum length of the tuple after substitution; a tuple with
  /// zero or one scalar elements is unwrapped if it would otherwise be
  /// a one-element tuple after substitution.
  unsigned getNumScalarElements() const;

  /// getElements - Return the elements of this tuple.
  ArrayRef<TupleTypeElt> getElements() const {
    return {getTrailingObjects<TupleTypeElt>(), getNumElements()};
  }

  const TupleTypeElt &getElement(unsigned i) const {
    return getTrailingObjects<TupleTypeElt>()[i];
  }

  /// getElementType - Return the type of the specified element.
  Type getElementType(unsigned ElementNo) const {
    return getTrailingObjects<TupleTypeElt>()[ElementNo].getType();
  }

  TupleEltTypeArrayRef getElementTypes() const {
    return TupleEltTypeArrayRef(getElements());
  }
  
  /// getNamedElementId - If this tuple has an element with the specified name,
  /// return the element index, otherwise return -1.
  int getNamedElementId(Identifier I) const;

  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::Tuple;
  }

  void Profile(llvm::FoldingSetNodeID &ID) {
    Profile(ID, getElements());
  }
  static void Profile(llvm::FoldingSetNodeID &ID, 
                      ArrayRef<TupleTypeElt> Elements);
  
  bool containsPackExpansionType() const;

  /// Check whether this tuple consists of a single unlabeled element
  /// of \c PackExpansionType.
  bool isSingleUnlabeledPackExpansion() const;

private:
  TupleType(ArrayRef<TupleTypeElt> elements, const ASTContext *CanCtx,
            RecursiveTypeProperties properties)
      : TypeBase(TypeKind::Tuple, CanCtx, properties) {
    Bits.TupleType.Count = elements.size();
    std::uninitialized_copy(elements.begin(), elements.end(),
                            getTrailingObjects<TupleTypeElt>());
  }
};
BEGIN_CAN_TYPE_WRAPPER(TupleType, Type)
  CanType getElementType(unsigned elementNo) const {
    return CanType(getPointer()->getElementType(elementNo));
  }
  CanTupleEltTypeArrayRef getElementTypes() const {
    return CanTupleEltTypeArrayRef(getPointer()->getElements());
  }

  bool containsPackExpansionType() const {
    return containsPackExpansionTypeImpl(*this);
  }

  /// Induce a pack type from the elements of this tuple type.
  inline CanTypeWrapper<PackType> getInducedPackType() const;

  /// Induce a pack type from a range of the elements of this tuple type.
  inline CanTypeWrapper<PackType>
  getInducedPackType(unsigned start, unsigned count) const;

  /// Induce a formal pack type with the same shape as the elements
  /// of this lowered tuple type, making a best effort to use the same
  /// element types.
  inline CanTypeWrapper<PackType>
  getInducedApproximateFormalPackType() const;

private:
  static bool containsPackExpansionTypeImpl(CanTupleType tuple);

  static CanTypeWrapper<PackType>
  getInducedPackTypeImpl(CanTupleType tuple);

  static CanTypeWrapper<PackType>
  getInducedPackTypeImpl(CanTupleType tuple, unsigned start, unsigned count);

  static CanTypeWrapper<PackType>
  getInducedApproximateFormalPackTypeImpl(CanTupleType tuple);
END_CAN_TYPE_WRAPPER(TupleType, Type)

/// UnboundGenericType - Represents a generic type where the type arguments have
/// not yet been resolved.
///
/// This type is on its way out. Try to avoid introducing new usages.
class UnboundGenericType : public AnyGenericType,
    public llvm::FoldingSetNode {
private:
  UnboundGenericType(GenericTypeDecl *TheDecl, Type Parent, const ASTContext &C,
                     RecursiveTypeProperties properties)
    : AnyGenericType(TheDecl, Parent, TypeKind::UnboundGeneric,
                     (!Parent || Parent->isCanonical()) ? &C : nullptr,
                     properties | RecursiveTypeProperties::HasUnboundGeneric) {}

public:
  static UnboundGenericType* get(GenericTypeDecl *TheDecl, Type Parent,
                                 const ASTContext &C);

  void Profile(llvm::FoldingSetNodeID &ID) {
    Profile(ID, getDecl(), getParent());
  }
  static void Profile(llvm::FoldingSetNodeID &ID, GenericTypeDecl *D,
                      Type Parent);

  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::UnboundGeneric;
  }
};
DEFINE_EMPTY_CAN_TYPE_WRAPPER(UnboundGenericType, AnyGenericType)

inline CanType getAsCanType(const Type &type) { return CanType(type); }
typedef ArrayRefView<Type,CanType,getAsCanType> CanTypeArrayRef;

/// BoundGenericType - An abstract class for applying a generic type to the
/// given type arguments.
class BoundGenericType : public NominalOrBoundGenericNominalType,
    public llvm::FoldingSetNode {
  
  /// Retrieve the intrusive pointer storage from the subtype
  const Type *getTrailingObjectsPointer() const;
  Type *getTrailingObjectsPointer() {
    const BoundGenericType *temp = this;
    return const_cast<Type *>(temp->getTrailingObjectsPointer());
  }

protected:
  BoundGenericType(TypeKind theKind, NominalTypeDecl *theDecl, Type parent,
                   ArrayRef<Type> genericArgs, const ASTContext *context,
                   RecursiveTypeProperties properties);

public:
  static BoundGenericType* get(NominalTypeDecl *TheDecl, Type Parent,
                               ArrayRef<Type> GenericArgs);

  /// Retrieve the set of generic arguments provided at this level.
  ArrayRef<Type> getGenericArgs() const {
    return {getTrailingObjectsPointer(), Bits.BoundGenericType.GenericArgCount};
  }

  SmallVector<Type, 2> getExpandedGenericArgs();

  void Profile(llvm::FoldingSetNodeID &ID) {
    Profile(ID, getDecl(), getParent(), getGenericArgs());
  }
  static void Profile(llvm::FoldingSetNodeID &ID, NominalTypeDecl *TheDecl,
                      Type Parent, ArrayRef<Type> GenericArgs);

  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() >= TypeKind::First_BoundGenericType &&
           T->getKind() <= TypeKind::Last_BoundGenericType;
  }
};
BEGIN_CAN_TYPE_WRAPPER(BoundGenericType, NominalOrBoundGenericNominalType)
  CanTypeArrayRef getGenericArgs() const {
    return CanTypeArrayRef(getPointer()->getGenericArgs());
  }
END_CAN_TYPE_WRAPPER(BoundGenericType, NominalOrBoundGenericNominalType)


/// BoundGenericClassType - A subclass of BoundGenericType for the case
/// when the nominal type is a generic class type.
class BoundGenericClassType final : public BoundGenericType,
    private llvm::TrailingObjects<BoundGenericClassType, Type> {
  friend TrailingObjects;

private:
  BoundGenericClassType(ClassDecl *theDecl, Type parent,
                        ArrayRef<Type> genericArgs, const ASTContext *context,
                        RecursiveTypeProperties properties)
    : BoundGenericType(TypeKind::BoundGenericClass,
                       reinterpret_cast<NominalTypeDecl*>(theDecl), parent,
                       genericArgs, context, properties) {}
  friend class BoundGenericType;

public:
  static BoundGenericClassType* get(ClassDecl *theDecl, Type parent,
                                    ArrayRef<Type> genericArgs) {
    return cast<BoundGenericClassType>(
             BoundGenericType::get(reinterpret_cast<NominalTypeDecl*>(theDecl),
                                   parent, genericArgs));
  }

  /// Returns the declaration that declares this type.
  ClassDecl *getDecl() const {
    return reinterpret_cast<ClassDecl*>(BoundGenericType::getDecl());
  }

  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::BoundGenericClass;
  }
};
DEFINE_EMPTY_CAN_TYPE_WRAPPER(BoundGenericClassType, BoundGenericType)

/// BoundGenericEnumType - A subclass of BoundGenericType for the case
/// when the nominal type is a generic enum type.
class BoundGenericEnumType final : public BoundGenericType,
    private llvm::TrailingObjects<BoundGenericEnumType, Type> {
  friend TrailingObjects;

private:
  BoundGenericEnumType(EnumDecl *theDecl, Type parent,
                       ArrayRef<Type> genericArgs, const ASTContext *context,
                       RecursiveTypeProperties properties)
    : BoundGenericType(TypeKind::BoundGenericEnum,
                       reinterpret_cast<NominalTypeDecl*>(theDecl), parent,
                       genericArgs, context, properties) {}
  friend class BoundGenericType;

public:
  static BoundGenericEnumType* get(EnumDecl *theDecl, Type parent,
                                    ArrayRef<Type> genericArgs) {
    return cast<BoundGenericEnumType>(
             BoundGenericType::get(reinterpret_cast<NominalTypeDecl*>(theDecl),
                                   parent, genericArgs));
  }

  /// Returns the declaration that declares this type.
  EnumDecl *getDecl() const {
    return reinterpret_cast<EnumDecl*>(BoundGenericType::getDecl());
  }

  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::BoundGenericEnum;
  }
};
DEFINE_EMPTY_CAN_TYPE_WRAPPER(BoundGenericEnumType, BoundGenericType)

/// BoundGenericStructType - A subclass of BoundGenericType for the case
/// when the nominal type is a generic struct type.
class BoundGenericStructType final : public BoundGenericType,
    private llvm::TrailingObjects<BoundGenericStructType, Type> {
  friend TrailingObjects;

private:
  BoundGenericStructType(StructDecl *theDecl, Type parent,
                         ArrayRef<Type> genericArgs, const ASTContext *context,
                         RecursiveTypeProperties properties)
    : BoundGenericType(TypeKind::BoundGenericStruct, 
                       reinterpret_cast<NominalTypeDecl*>(theDecl), parent,
                       genericArgs, context, properties) {}
  friend class BoundGenericType;

public:
  static BoundGenericStructType* get(StructDecl *theDecl, Type parent,
                                    ArrayRef<Type> genericArgs) {
    return cast<BoundGenericStructType>(
             BoundGenericType::get(reinterpret_cast<NominalTypeDecl*>(theDecl),
                                   parent, genericArgs));
  }

  /// Returns the declaration that declares this type.
  StructDecl *getDecl() const {
    return reinterpret_cast<StructDecl*>(BoundGenericType::getDecl());
  }

  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::BoundGenericStruct;
  }
};
DEFINE_EMPTY_CAN_TYPE_WRAPPER(BoundGenericStructType, BoundGenericType)

/// NominalType - Represents a type with a name that is significant, such that
/// the name distinguishes it from other structurally-similar types that have
/// different names. Nominal types are always canonical.
class NominalType : public NominalOrBoundGenericNominalType {

protected:
  NominalType(TypeKind K, const ASTContext *C, NominalTypeDecl *TheDecl,
              Type Parent, RecursiveTypeProperties properties)
    : NominalOrBoundGenericNominalType(TheDecl, Parent, K,
               (!Parent || Parent->isCanonical())? C : nullptr, properties) {}

public:
  static NominalType *get(NominalTypeDecl *D, Type Parent, const ASTContext &C);

  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() >= TypeKind::First_NominalType &&
           T->getKind() <= TypeKind::Last_NominalType;
  }
};
DEFINE_EMPTY_CAN_TYPE_WRAPPER(NominalType, NominalOrBoundGenericNominalType)

/// EnumType - This represents the type declared by an EnumDecl.
class EnumType : public NominalType {
public:
  /// getDecl() - Returns the decl which declares this type.
  EnumDecl *getDecl() const {
    return reinterpret_cast<EnumDecl *>(NominalType::getDecl());
  }

  /// Retrieve the type when we're referencing the given enum
  /// declaration in the parent type \c Parent.
  static EnumType *get(EnumDecl *D, Type Parent, const ASTContext &C);

  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::Enum;
  }

private:
  EnumType(EnumDecl *TheDecl, Type Parent, const ASTContext &Ctx,
            RecursiveTypeProperties properties);
};
DEFINE_EMPTY_CAN_TYPE_WRAPPER(EnumType, NominalType)

/// StructType - This represents the type declared by a StructDecl.
class StructType : public NominalType {
public:
  /// getDecl() - Returns the decl which declares this type.
  StructDecl *getDecl() const {
    return reinterpret_cast<StructDecl *>(NominalType::getDecl());
  }

  /// Retrieve the type when we're referencing the given struct
  /// declaration in the parent type \c Parent.
  static StructType *get(StructDecl *D, Type Parent, const ASTContext &C);

  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::Struct;
  }
  
private:
  StructType(StructDecl *TheDecl, Type Parent, const ASTContext &Ctx,
             RecursiveTypeProperties properties);
};
DEFINE_EMPTY_CAN_TYPE_WRAPPER(StructType, NominalType)

/// ClassType - This represents the type declared by a ClassDecl.
class ClassType : public NominalType {
public:
  /// getDecl() - Returns the decl which declares this type.
  ClassDecl *getDecl() const {
    return reinterpret_cast<ClassDecl *>(NominalType::getDecl());
  }

  /// Retrieve the type when we're referencing the given class
  /// declaration in the parent type \c Parent.
  static ClassType *get(ClassDecl *D, Type Parent, const ASTContext &C);

  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::Class;
  }
  
private:
  ClassType(ClassDecl *TheDecl, Type Parent, const ASTContext &Ctx,
            RecursiveTypeProperties properties);
};
DEFINE_EMPTY_CAN_TYPE_WRAPPER(ClassType, NominalType)

/// Describes the representation of a metatype.
///
/// There are several potential representations for metatypes within
/// SIL, which are distinguished by the metatype representation. This
/// enumeration captures the different representations. Some
/// conversions between representations are possible: for example, one
/// can convert a thin representation to a thick one (but not
/// vice-versa), and different representations are required in
/// different places.
enum class MetatypeRepresentation : char {
  /// A thin metatype requires no runtime information, because the
  /// type itself provides no dynamic behavior.
  ///
  /// Struct and enum metatypes are thin, because dispatch to static
  /// struct and enum members is completely static.
  Thin,
  /// A thick metatype refers to a complete metatype representation
  /// that allows introspection and dynamic dispatch. 
  ///
  /// Thick metatypes are used for class and existential metatypes,
  /// which permit dynamic behavior.
  Thick,
  /// An Objective-C metatype refers to an Objective-C class object.
  ObjC,

  Last_MetatypeRepresentation = ObjC
};

/// AnyMetatypeType - A common parent class of MetatypeType and
/// ExistentialMetatypeType.
class AnyMetatypeType : public TypeBase {
  Type InstanceType;
protected:
  AnyMetatypeType(TypeKind kind, const ASTContext *C,
                  RecursiveTypeProperties properties,
                  Type instanceType,
                  Optional<MetatypeRepresentation> repr);


public:
  Type getInstanceType() const { return InstanceType; }

  /// Does this metatype have a representation?
  ///
  /// Only SIL metatype types have a representation.
  bool hasRepresentation() const {
    return Bits.AnyMetatypeType.Representation > 0;
  }
  
  /// Retrieve the metatype representation.
  ///
  /// The metatype representation is a SIL-only property. Thin
  /// metatypes can be lowered away to empty types in IR, unless a
  /// metatype value is required at an abstraction level.
  MetatypeRepresentation getRepresentation() const {
    assert(Bits.AnyMetatypeType.Representation &&
           "metatype has no representation");
    return static_cast<MetatypeRepresentation>(
             Bits.AnyMetatypeType.Representation - 1);
  }

  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::Metatype ||
           T->getKind() == TypeKind::ExistentialMetatype;
  }
};
BEGIN_CAN_TYPE_WRAPPER(AnyMetatypeType, Type)
  PROXY_CAN_TYPE_SIMPLE_GETTER(getInstanceType)
END_CAN_TYPE_WRAPPER(AnyMetatypeType, Type)

/// MetatypeType - This is the type given to a metatype value.  When a type is
/// declared, a 'metatype' value is injected into the value namespace to
/// resolve references to the type.  An example:
///
///  struct x { ... }  // declares type 'x' and metatype 'x'.
///  x.a()             // use of the metatype value since its a value context.
///
/// In general, this is spelled X.Type, unless X is an existential
/// type, in which case the ordinary metatype is spelled X.Protocol
/// and X.Type connotes the ExistentialMetatypeType.
class MetatypeType : public AnyMetatypeType {
  static MetatypeType *get(Type T, Optional<MetatypeRepresentation> Repr,
                           const ASTContext &C);

public:
  /// Return the MetatypeType for the specified type declaration.
  ///
  /// This leaves the 'representation' property unavailable.
  static MetatypeType *get(Type T, const ASTContext &C) {
    return get(T, None, C);
  }
  
  /// Return the MetatypeType for the specified type declaration with
  /// the given representation.
  ///
  /// Metatype representation is a SIL-only property. Thin metatypes
  /// can be lowered away to empty types in IR.
  static MetatypeType *get(Type T,
                           Optional<MetatypeRepresentation> repr = None) {
    return get(T, repr, T->getASTContext());
  }

  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::Metatype;
  }
  
private:
  MetatypeType(Type T, const ASTContext *C,
               RecursiveTypeProperties properties,
               Optional<MetatypeRepresentation> repr);
  friend class TypeDecl;
};
BEGIN_CAN_TYPE_WRAPPER(MetatypeType, AnyMetatypeType)
  static CanMetatypeType get(CanType type) {
    return CanMetatypeType(MetatypeType::get(type));
  }
  static CanMetatypeType get(CanType type, MetatypeRepresentation repr) {
    return CanMetatypeType(MetatypeType::get(type, repr));
  }
END_CAN_TYPE_WRAPPER(MetatypeType, AnyMetatypeType)

/// ExistentialMetatypeType - This is the type given to an existential
/// metatype value, i.e. the type of the dynamic type of an
/// existential value.  The instance type must be an existential type
/// of some sort.
///
/// Formally, this type is \exists t : T... . t.Type.  In contrast,
/// the MetatypeType for a ProtocolType is a singleton.
///
/// This is spelled X.Type, where X is an existential type.
///
/// The representation of an existential metatype cannot be thin.
class ExistentialMetatypeType : public AnyMetatypeType {
public:
  static ExistentialMetatypeType *get(Type T,
                                      Optional<MetatypeRepresentation> Repr,
                                      const ASTContext &C);

  /// Return the ExistentialMetatypeType for the specified type
  /// with the given representation.
  ///
  /// Metatype representation is a SIL-only property. Existential
  /// metatypes cannot be thin.
  static ExistentialMetatypeType *get(Type T,
                                      Optional<MetatypeRepresentation> repr
                                        = None) {
    return get(T, repr, T->getASTContext());
  }

  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::ExistentialMetatype;
  }

  Type getExistentialInstanceType();
  
private:
  ExistentialMetatypeType(Type T, const ASTContext *C,
                          RecursiveTypeProperties properties,
                          Optional<MetatypeRepresentation> repr);
  friend class TypeDecl;
};
BEGIN_CAN_TYPE_WRAPPER(ExistentialMetatypeType, AnyMetatypeType)
  static CanExistentialMetatypeType get(CanType type) {
    return CanExistentialMetatypeType(ExistentialMetatypeType::get(type));
  }
  static CanExistentialMetatypeType get(CanType type,
                                        MetatypeRepresentation repr) {
    return CanExistentialMetatypeType(ExistentialMetatypeType::get(type, repr));
  }
  PROXY_CAN_TYPE_SIMPLE_GETTER(getExistentialInstanceType)
END_CAN_TYPE_WRAPPER(ExistentialMetatypeType, AnyMetatypeType)
  
/// ModuleType - This is the type given to a module value, e.g. the "Builtin" in
/// "Builtin.int".  This is typically given to a ModuleExpr, but can also exist
/// on ParenExpr, for example.
class ModuleType : public TypeBase {
  ModuleDecl *const TheModule;
  
public:
  /// get - Return the ModuleType for the specified module.
  static ModuleType *get(ModuleDecl *M);

  ModuleDecl *getModule() const { return TheModule; }

  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::Module;
  }
  
private:
  ModuleType(ModuleDecl *M, const ASTContext &Ctx)
    : TypeBase(TypeKind::Module, &Ctx, // Always canonical
               RecursiveTypeProperties()),
      TheModule(M) {
  }
};
DEFINE_EMPTY_CAN_TYPE_WRAPPER(ModuleType, Type)
  
/// The type given to a dynamic \c Self return type.
///
/// Example:
/// \code
/// class X {
///   class func factory() -> Self { ... }
/// };
/// \endcode
///
/// In this example, \c Self is represented by a 
/// \c DynamicSelfType node whose self type is \c X.
class DynamicSelfType : public TypeBase {
  Type SelfType;

public:
  /// Return the DynamicSelf for the specified self type.
  static DynamicSelfType *get(Type selfType, const ASTContext &ctx);

  /// Retrieve the (static) self type for this dynamic self type.
  Type getSelfType() const { return SelfType; }

  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::DynamicSelf;
  }
  
private:
  DynamicSelfType(Type selfType, const ASTContext &ctx,
                  RecursiveTypeProperties properties)
    : TypeBase(TypeKind::DynamicSelf, selfType->isCanonical()? &ctx : 0,
               properties | RecursiveTypeProperties(
                 RecursiveTypeProperties::HasDynamicSelf)),
      SelfType(selfType) { }

  friend class TypeDecl;
};
BEGIN_CAN_TYPE_WRAPPER(DynamicSelfType, Type)
  PROXY_CAN_TYPE_SIMPLE_GETTER(getSelfType)

  static CanDynamicSelfType get(CanType selfType, const ASTContext &ctx) {
    return CanDynamicSelfType(DynamicSelfType::get(selfType, ctx));
  }
END_CAN_TYPE_WRAPPER(DynamicSelfType, Type)

/// AnyFunctionType - A function type has zero or more input parameters and a
/// single result. The result type may be a tuple. For example:
///   "(int) -> int" or "(a : int, b : int) -> (int, int)".
///
/// There are two kinds of function types:  monomorphic (FunctionType) and
/// polymorphic (GenericFunctionType). Both type families additionally can
/// be 'thin', indicating that a function value has no capture context and can be
/// represented at the binary level as a single function pointer.
class AnyFunctionType : public TypeBase {
  const Type Output;
  
public:
  using Representation = FunctionTypeRepresentation;

  class Param {
  public:
    explicit Param(Type t,
                   Identifier l = Identifier(),
                   ParameterTypeFlags f = ParameterTypeFlags(),
                   Identifier internalLabel = Identifier())
        : Ty(t), Label(l), InternalLabel(internalLabel), Flags(f) {
      assert(t && "param type must be non-null");
      assert(!t->is<InOutType>() && "set flags instead");
    }

  private:
    /// The type of the parameter. For a variadic parameter, this is the
    /// element type.
    Type Ty;
    
    /// The label associated with the parameter, if any.
    Identifier Label;

    /// The internal label of the parameter, if explicitly specified, otherwise
    /// empty. The internal label is considered syntactic sugar. It is not
    /// considered part of the canonical type and is thus also ignored in \c
    /// operator==.
    /// E.g.
    ///  - `name name2: Int` has internal label `name2`
    ///  - `_ name2: Int` has internal label `name2`
    ///  - `name: Int` has no internal label
    Identifier InternalLabel;
    
    /// Parameter specific flags.
    ParameterTypeFlags Flags = {};
    
  public:
    /// FIXME: Remove this. Return the formal type of the parameter in the
    /// function type, including the InOutType if there is one.
    ///
    /// For example, 'inout Int' => 'inout Int', 'Int...' => 'Int'.
    Type getOldType() const;

    /// Return the formal type of the parameter.
    ///
    /// For example, 'inout Int' => 'Int', 'Int...' => 'Int'.
    Type getPlainType() const { return Ty; }

    /// The type of the parameter when referenced inside the function body
    /// as an rvalue.
    ///
    /// For example, 'inout Int' => 'Int', 'Int...' => '[Int]'.
    Type getParameterType(bool forCanonical = false,
                          ASTContext *ctx = nullptr) const;

    bool hasLabel() const { return !Label.empty(); }
    Identifier getLabel() const { return Label; }

    bool hasInternalLabel() const { return !InternalLabel.empty(); }
    Identifier getInternalLabel() const { return InternalLabel; }

    /// Return true if argument name is valid and matches \c paramName.
    ///
    /// The three tests to check if argument name is valid are:
    /// 1. allow argument if it matches \c paramName,
    /// 2. allow argument if  $_  for omitted projected value label,
    /// 3. allow argument if it matches \c paramName without its \c $ prefix.
    bool matchParameterLabel(Identifier const &paramName) const {
      auto argLabel = getLabel();
      if (argLabel == paramName)
        return true;
      if ((argLabel.str() == "$_") && paramName.empty())
        return true;
      if (argLabel.hasDollarPrefix() && argLabel.str().drop_front() == paramName.str())
        return true;
      return false;
    }

    Param getCanonical(CanGenericSignature genericSig) const;
    
    ParameterTypeFlags getParameterFlags() const { return Flags; }

    /// Whether the parameter is varargs
    bool isVariadic() const { return Flags.isVariadic(); }
    
    /// Whether the parameter is marked '@autoclosure'
    bool isAutoClosure() const { return Flags.isAutoClosure(); }
    
    /// Whether the parameter is marked 'inout'
    bool isInOut() const { return Flags.isInOut(); }
    
    /// Whether the parameter is marked 'shared'
    bool isShared() const { return Flags.isShared(); }

    /// Whether the parameter is marked 'owned'
    bool isOwned() const { return Flags.isOwned(); }

    /// Whether the parameter is marked '@_nonEphemeral'
    bool isNonEphemeral() const { return Flags.isNonEphemeral(); }

    /// Whether the parameter is 'isolated'.
    bool isIsolated() const { return Flags.isIsolated(); }

    /// Whether the parameter is 'isCompileTimeConst'.
    bool isCompileTimeConst() const { return Flags.isCompileTimeConst(); }

    /// Whether the parameter is marked '@noDerivative'.
    bool isNoDerivative() const { return Flags.isNoDerivative(); }

    ValueOwnership getValueOwnership() const {
      return Flags.getValueOwnership();
    }

    /// Returns \c true if the two \c Params are equal in their canonicalized
    /// form.
    /// Two \c Params are equal if their external label, flags and
    /// *canonicalized* types match. The internal label and sugar types are
    /// *not* considered for type equality.
    bool operator==(Param const &b) const {
      return (Label == b.Label &&
              getPlainType()->isEqual(b.getPlainType()) &&
              Flags == b.Flags);
    }
    bool operator!=(Param const &b) const { return !(*this == b); }

    /// Return the parameter without external and internal labels.
    Param getWithoutLabels() const {
      return Param(Ty, /*Label=*/Identifier(), Flags,
                   /*InternalLabel=*/Identifier());
    }

    Param withLabel(Identifier newLabel) const {
      return Param(Ty, newLabel, Flags, InternalLabel);
    }

    Param withType(Type newType) const {
      return Param(newType, Label, Flags, InternalLabel);
    }

    Param withFlags(ParameterTypeFlags flags) const {
      return Param(Ty, Label, flags, InternalLabel);
    }
  };

  class CanParam : public Param {
    explicit CanParam(const Param &param) : Param(param) {}
  public:
    static CanParam getFromParam(const Param &param) { return CanParam(param); }

    CanType getOldType() const { return CanType(Param::getOldType()); }
    CanType getPlainType() const { return CanType(Param::getPlainType()); }
    CanType getParameterType() const {
      return CanType(Param::getParameterType(/*forCanonical*/ true));
    }
  };

  using ExtInfo = swift::ASTExtInfo;
  using ExtInfoBuilder = swift::ASTExtInfoBuilder;
  using CanParamArrayRef =
    ArrayRefView<Param,CanParam,CanParam::getFromParam,/*AccessOriginal*/true>;
  
  class CanYield;
  class Yield {
    Type Ty;
    YieldTypeFlags Flags;
  public:
    explicit Yield(Type type, YieldTypeFlags flags)
      : Ty(type), Flags(flags) {}

    Type getType() const { return Ty; }

    YieldTypeFlags getFlags() const { return Flags; }
    ValueOwnership getValueOwnership() const {
      return getFlags().getValueOwnership();
    }
    bool isInOut() const { return getFlags().isInOut(); }

    CanYield getCanonical() const;

    /// There are a number of places where it's convenient to re-use
    /// the call machinery, processing yields as if they were
    /// parameters of a call.  Return this reinterpreted as a parameter.
    Param asParam() const {
      return Param(getType(), Identifier(), getFlags().asParamFlags());
    }

    Yield subst(SubstitutionMap subs, SubstOptions options=None) const {
      return Yield(getType().subst(subs, options), getFlags());
    }

    bool operator==(const Yield &other) const {
      return getType()->isEqual(other.getType()) &&
             getFlags() == other.getFlags();
    }
    bool operator!=(const Yield &other) const {
      return !operator==(other);
    }
  };

  class CanYield : public Yield {
  public:
    explicit CanYield(CanType type, YieldTypeFlags flags)
      : Yield(type, flags) {}

    CanType getType() const { return CanType(Yield::getType()); }
    CanParam asParam() const { return CanParam::getFromParam(Yield::asParam());}

    CanYield subst(SubstitutionMap subs, SubstOptions options=None) const {
      return CanYield(getType().subst(subs, options)->getCanonicalType(),
                      getFlags());
    }
  };

protected:
  /// Create an AnyFunctionType.
  ///
  /// Subclasses are responsible for storing and retrieving the
  /// ClangTypeInfo value if one is present.
  AnyFunctionType(TypeKind Kind, const ASTContext *CanTypeContext, Type Output,
                  RecursiveTypeProperties properties, unsigned NumParams,
                  Optional<ExtInfo> Info)
      : TypeBase(Kind, CanTypeContext, properties), Output(Output) {
    if (Info.has_value()) {
      Bits.AnyFunctionType.HasExtInfo = true;
      Bits.AnyFunctionType.ExtInfoBits = Info.value().getBits();
      Bits.AnyFunctionType.HasClangTypeInfo =
          !Info.value().getClangTypeInfo().empty();
      Bits.AnyFunctionType.HasGlobalActor =
          !Info.value().getGlobalActor().isNull();
      // The use of both assert() and static_assert() is intentional.
      assert(Bits.AnyFunctionType.ExtInfoBits == Info.value().getBits() &&
             "Bits were dropped!");
      static_assert(
          ASTExtInfoBuilder::NumMaskBits == NumAFTExtInfoBits,
          "ExtInfo and AnyFunctionTypeBitfields must agree on bit size");
    } else {
      Bits.AnyFunctionType.HasExtInfo = false;
      Bits.AnyFunctionType.HasClangTypeInfo = false;
      Bits.AnyFunctionType.ExtInfoBits = 0;
      Bits.AnyFunctionType.HasGlobalActor = false;
    }
    Bits.AnyFunctionType.NumParams = NumParams;
    assert(Bits.AnyFunctionType.NumParams == NumParams && "Params dropped!");
  }

public:
  /// Take an array of parameters and turn it into a tuple or paren type.
  ///
  /// \param paramFlagHandling How to handle the parameter flags.
  static Type composeTuple(ASTContext &ctx, ArrayRef<Param> params,
                           ParameterFlagHandling paramFlagHandling);

  /// Given two arrays of parameters determine if they are equal in their
  /// canonicalized form. Internal labels and type sugar is *not* taken into
  /// account.
  static bool equalParams(ArrayRef<Param> a, ArrayRef<Param> b);

  /// Given two arrays of parameters determine if they are equal in their
  /// canonicalized form. Internal labels and type sugar is *not* taken into
  /// account.
  static bool equalParams(CanParamArrayRef a, CanParamArrayRef b);

  /// Given an array of parameters and an argument list of the
  /// same length, update each parameter to have the corresponding label.
  /// The internal parameter labels remain the same.
  static void relabelParams(MutableArrayRef<Param> params,
                            ArgumentList *argList);

  Type getResult() const { return Output; }
  ArrayRef<Param> getParams() const;
  unsigned getNumParams() const { return Bits.AnyFunctionType.NumParams; }

  GenericSignature getOptGenericSignature() const;
  
  bool hasClangTypeInfo() const {
    return Bits.AnyFunctionType.HasClangTypeInfo;
  }

  bool hasGlobalActor() const {
    return Bits.AnyFunctionType.HasGlobalActor;
  }

  ClangTypeInfo getClangTypeInfo() const;
  ClangTypeInfo getCanonicalClangTypeInfo() const;

  Type getGlobalActor() const;

  /// Returns true if the function type stores a Clang type that cannot
  /// be derived from its Swift type. Returns false otherwise, including if
  /// the function type is not @convention(c) or @convention(block).
  ///
  /// For example, if you have a function pointer from C getting imported with
  /// the following type:
  ///
  /// @convention(c, cType: "void (*)(size_t (*)(size_t))")
  ///   (@convention(c, cType: "size_t (*)(size_t)") (Int) -> Int)) -> Void
  ///
  /// The parameter's function type will have hasNonDerivableClangType() = true,
  /// but the outer function type will have hasNonDerivableClangType() = false,
  /// because the parameter and result type are sufficient to correctly derive
  /// the Clang type for the outer function type. In terms of mangling,
  /// the parameter type's mangling will incorporate the Clang type but the
  /// outer function type's mangling doesn't need to duplicate that information.
  bool hasNonDerivableClangType();

  bool hasExtInfo() const { return Bits.AnyFunctionType.HasExtInfo; }

  ExtInfo getExtInfo() const {
    assert(hasExtInfo());
    return ExtInfo(Bits.AnyFunctionType.ExtInfoBits, getClangTypeInfo(),
                   getGlobalActor());
  }

  /// Get the canonical ExtInfo for the function type.
  ///
  /// The parameter useClangFunctionType is present only for staging purposes.
  /// In the future, we will always use the canonical clang function type.
  ExtInfo getCanonicalExtInfo(bool useClangFunctionType) const {
    assert(hasExtInfo());
    Type globalActor = getGlobalActor();
    if (globalActor)
      globalActor = globalActor->getCanonicalType();
    return ExtInfo(Bits.AnyFunctionType.ExtInfoBits,
                   useClangFunctionType ? getCanonicalClangTypeInfo()
                                        : ClangTypeInfo(),
                   globalActor);
  }

  bool hasSameExtInfoAs(const AnyFunctionType *otherFn);

  /// Get the representation of the function type.
  Representation getRepresentation() const {
    return getExtInfo().getRepresentation();
  }

  /// Appends the parameters indicated by `parameterIndices` to `results`.
  ///
  /// For curried function types: if `reverseCurryLevels` is true, append
  /// the `self` parameter last instead of first.
  ///
  /// TODO(TF-874): Simplify logic and remove the `reverseCurryLevels` flag.
  void getSubsetParameters(IndexSubset *parameterIndices,
                           SmallVectorImpl<AnyFunctionType::Param> &results,
                           bool reverseCurryLevels = false);

  /// Returns the derivative function type for the given parameter indices,
  /// result index, derivative function kind, derivative function generic
  /// signature (optional), and other auxiliary parameters.
  ///
  /// Preconditions:
  /// - Parameters corresponding to parameter indices must conform to
  ///   `Differentiable`.
  /// - There is one semantic function result type: either the formal original
  ///   result or an `inout` parameter. It must conform to `Differentiable`.
  ///
  /// Typing rules, given:
  /// - Original function type. Three cases:
  ///   - Top-level function: `(T0, T1, ...) -> R`
  ///   - Static method: `(Self.Type) -> (T0, T1, ...) -> R`
  ///   - Instance method: `(Self) -> (T0, T1, ...) -> R`
  ///
  /// Terminology:
  /// - The derivative of a `Differentiable`-conforming type has the
  ///   `TangentVector` associated type. `TangentVector` is abbreviated as `Tan`
  ///   below.
  /// - "wrt" parameters refers to differentiability parameters, identified by
  ///   the parameter indices.
  /// - "wrt" result refers to the result identified by the result index.
  ///
  /// JVP derivative type:
  /// - Takes original parameters.
  /// - Returns original result, followed by a differential function, which
  ///   takes "wrt" parameter derivatives and returns a "wrt" result derivative.
  ///
  /// \verbatim
  ///   (T0, T1, ...) -> (R,       (T0.Tan, T1.Tan, ...) -> R.Tan)
  ///                     ^         ^~~~~~~~~~~~~~~~~~~     ^~~~~
  ///           original result | derivatives wrt params | derivative wrt result
  ///
  ///   (Self) -> (T0, ...) -> (R, (Self.Tan, T0.Tan, ...) -> R.Tan)
  ///                           ^   ^~~~~~~~~~~~~~~~~~~~~     ^~~~~
  ///             original result  |  deriv. wrt params  |  deriv. wrt result
  /// \endverbatim
  ///
  /// VJP derivative type:
  /// - Takes original parameters.
  /// - Returns original result, followed by a pullback function, which
  ///   takes a "wrt" result derivative and returns "wrt" parameter derivatives.
  ///
  /// \verbatim
  ///   (T0, T1, ...) -> (R,           (R.Tan)    ->     (T0.Tan, T1.Tan, ...))
  ///                     ^             ^~~~~             ^~~~~~~~~~~~~~~~~~~
  ///          original result | derivative wrt result | derivatives wrt params
  ///
  ///   (Self) -> (T0, ...) -> (R,     (R.Tan)    ->    (Self.Tan, T0.Tan, ...))
  ///                           ^       ^~~~~            ^~~~~~~~~~~~~~~~~~~~~
  ///              original result | deriv. wrt result | deriv. wrt params
  /// \endverbatim
  ///
  /// The original type may have `inout` parameters. If so, the
  /// differential/pullback typing rules are more nuanced: see documentation for
  /// `getAutoDiffDerivativeFunctionLinearMapType` for details. Semantically,
  /// `inout` parameters behave as both parameters and results.
  ///
  /// By default, if the original type has a `self` parameter list and parameter
  /// indices include `self`, the computed derivative function type will return
  /// a linear map taking/returning self's tangent *last* instead of first, for
  /// consistency with SIL.
  ///
  /// If `makeSelfParamFirst` is true, `self`'s tangent is reordered to appear
  /// first. `makeSelfParamFirst` should be true when working with user-facing
  /// derivative function types, e.g. when type-checking `@differentiable` and
  /// `@derivative` attributes.
  AnyFunctionType *getAutoDiffDerivativeFunctionType(
      IndexSubset *parameterIndices, AutoDiffDerivativeFunctionKind kind,
      LookupConformanceFn lookupConformance,
      GenericSignature derivativeGenericSignature = GenericSignature(),
      bool makeSelfParamFirst = false);

  /// Returns the corresponding linear map function type for the given parameter
  /// indices, linear map function kind, and other auxiliary parameters.
  ///
  /// Preconditions:
  /// - Parameters corresponding to parameter indices must conform to
  ///   `Differentiable`.
  /// - There is one semantic function result type: either the formal original
  ///   result or an `inout` parameter. It must conform to `Differentiable`.
  ///
  /// Differential typing rules: takes "wrt" parameter derivatives and returns a
  /// "wrt" result derivative.
  ///
  /// - Case 1: original function has no `inout` parameters.
  ///   - Original:     `(T0, T1, ...) -> R`
  ///   - Differential: `(T0.Tan, T1.Tan, ...) -> R.Tan`
  /// - Case 2: original function has a non-wrt `inout` parameter.
  ///   - Original:     `(T0, inout T1, ...) -> Void`
  ///   - Differential: `(T0.Tan, ...) -> T1.Tan`
  /// - Case 3: original function has a wrt `inout` parameter.
  ///   - Original:     `(T0, inout T1, ...) -> Void`
  ///   - Differential: `(T0.Tan, inout T1.Tan, ...) -> Void`
  ///
  /// Pullback typing rules: takes a "wrt" result derivative and returns "wrt"
  /// parameter derivatives.
  ///
  /// - Case 1: original function has no `inout` parameters.
  ///   - Original: `(T0, T1, ...) -> R`
  ///   - Pullback: `R.Tan -> (T0.Tan, T1.Tan, ...)`
  /// - Case 2: original function has a non-wrt `inout` parameter.
  ///   - Original: `(T0, inout T1, ...) -> Void`
  ///   - Pullback: `(T1.Tan) -> (T0.Tan, ...)`
  /// - Case 3: original function has a wrt `inout` parameter.
  ///   - Original: `(T0, inout T1, ...) -> Void`
  ///   - Pullback: `(inout T1.Tan) -> (T0.Tan, ...)`
  ///
  /// If `makeSelfParamFirst` is true, `self`'s tangent is reordered to appear
  /// first. `makeSelfParamFirst` should be true when working with user-facing
  /// derivative function types, e.g. when type-checking `@differentiable` and
  /// `@derivative` attributes.
  llvm::Expected<AnyFunctionType *> getAutoDiffDerivativeFunctionLinearMapType(
      IndexSubset *parameterIndices, AutoDiffLinearMapKind kind,
      LookupConformanceFn lookupConformance, bool makeSelfParamFirst = false);

  AnyFunctionType *getWithoutDifferentiability() const;

  /// Return the function type without the throwing.
  AnyFunctionType *getWithoutThrowing() const;

  /// True if the parameter declaration it is attached to is guaranteed
  /// to not persist the closure for longer than the duration of the call.
  bool isNoEscape() const {
    return getExtInfo().isNoEscape();
  }

  bool isSendable() const {
    return getExtInfo().isSendable();
  }

  bool isAsync() const { return getExtInfo().isAsync(); }

  bool isThrowing() const { return getExtInfo().isThrowing(); }

  bool hasEffect(EffectKind kind) const;

  bool isDifferentiable() const { return getExtInfo().isDifferentiable(); }
  DifferentiabilityKind getDifferentiabilityKind() const {
    return getExtInfo().getDifferentiabilityKind();
  }

  /// Returns a new function type exactly like this one but with the ExtInfo
  /// replaced.
  AnyFunctionType *withExtInfo(ExtInfo info) const;

  static bool containsPackExpansionType(ArrayRef<Param> params);

  static void printParams(ArrayRef<Param> Params, raw_ostream &OS,
                          const PrintOptions &PO = PrintOptions());
  static void printParams(ArrayRef<Param> Params, ASTPrinter &Printer,
                          const PrintOptions &PO);

  static std::string getParamListAsString(ArrayRef<Param> Params,
                                          const PrintOptions &PO = PrintOptions());

  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() >= TypeKind::First_AnyFunctionType &&
           T->getKind() <= TypeKind::Last_AnyFunctionType;
  }
};
BEGIN_CAN_TYPE_WRAPPER(AnyFunctionType, Type)
  using ExtInfo = AnyFunctionType::ExtInfo;
  using ExtInfoBuilder = AnyFunctionType::ExtInfoBuilder;
  using CanParamArrayRef = AnyFunctionType::CanParamArrayRef;

  static CanAnyFunctionType get(CanGenericSignature signature,
                                CanParamArrayRef params,
                                CanType result,
                                Optional<ExtInfo> info = None);

  CanGenericSignature getOptGenericSignature() const;

  CanParamArrayRef getParams() const {
    return CanParamArrayRef(getPointer()->getParams());
  }

  PROXY_CAN_TYPE_SIMPLE_GETTER(getResult)
  
  CanAnyFunctionType withExtInfo(ExtInfo info) const {
    return CanAnyFunctionType(getPointer()->withExtInfo(info));
  }
END_CAN_TYPE_WRAPPER(AnyFunctionType, Type)

inline AnyFunctionType::CanYield AnyFunctionType::Yield::getCanonical() const {
  return CanYield(getType()->getCanonicalType(), getFlags());
}
/// FunctionType - A monomorphic function type, specified with an arrow.
///
/// For example:
///   let x : (Float, Int) -> Int
class FunctionType final
    : public AnyFunctionType,
      public llvm::FoldingSetNode,
      private llvm::TrailingObjects<FunctionType, AnyFunctionType::Param,
                                    ClangTypeInfo, Type> {
  friend TrailingObjects;


  size_t numTrailingObjects(OverloadToken<AnyFunctionType::Param>) const {
    return getNumParams();
  }

  size_t numTrailingObjects(OverloadToken<ClangTypeInfo>) const {
    return hasClangTypeInfo() ? 1 : 0;
  }

  size_t numTrailingObjects(OverloadToken<Type>) const {
    return hasGlobalActor() ? 1 : 0;
  }

public:
  /// 'Constructor' Factory Function
  static FunctionType *get(ArrayRef<Param> params, Type result,
                           Optional<ExtInfo> info = None);

  // Retrieve the input parameters of this function type.
  ArrayRef<Param> getParams() const {
    return {getTrailingObjects<Param>(), getNumParams()};
  }

  ClangTypeInfo getClangTypeInfo() const {
    if (!hasClangTypeInfo())
      return ClangTypeInfo();
    auto *info = getTrailingObjects<ClangTypeInfo>();
    assert(!info->empty() &&
           "If the ClangTypeInfo was empty, we shouldn't have stored it.");
    return *info;
  }

  Type getGlobalActor() const {
    if (!hasGlobalActor())
      return Type();
    return *getTrailingObjects<Type>();
  }

  void Profile(llvm::FoldingSetNodeID &ID) {
    Optional<ExtInfo> info = None;
    if (hasExtInfo())
      info = getExtInfo();
    Profile(ID, getParams(), getResult(), info);
  }
  static void Profile(llvm::FoldingSetNodeID &ID,
                      ArrayRef<Param> params,
                      Type result,
                      Optional<ExtInfo> info);

  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::Function;
  }
      
private:
  FunctionType(ArrayRef<Param> params, Type result, Optional<ExtInfo> info,
               const ASTContext *ctx, RecursiveTypeProperties properties);
};
BEGIN_CAN_TYPE_WRAPPER(FunctionType, AnyFunctionType)
  static CanFunctionType get(CanParamArrayRef params, CanType result,
                             Optional<ExtInfo> info = None) {
    auto fnType = FunctionType::get(params.getOriginalArray(), result, info);
    return cast<FunctionType>(fnType->getCanonicalType());
  }

  CanFunctionType withExtInfo(ExtInfo info) const {
    return CanFunctionType(cast<FunctionType>(getPointer()->withExtInfo(info)));
  }
END_CAN_TYPE_WRAPPER(FunctionType, AnyFunctionType)

/// Provides information about the parameter list of a given declaration, including whether each parameter
/// has a default argument.
struct ParameterListInfo {
  SmallBitVector defaultArguments;
  SmallBitVector acceptsUnlabeledTrailingClosures;
  SmallBitVector propertyWrappers;
  SmallBitVector implicitSelfCapture;
  SmallBitVector inheritActorContext;
  SmallBitVector variadicGenerics;

public:
  ParameterListInfo() { }

  ParameterListInfo(ArrayRef<AnyFunctionType::Param> params,
                    const ValueDecl *paramOwner, bool skipCurriedSelf);

  /// Whether the parameter at the given index has a default argument.
  bool hasDefaultArgument(unsigned paramIdx) const;

  /// Whether the parameter accepts an unlabeled trailing closure argument
  /// according to the "forward-scan" rule.
  bool acceptsUnlabeledTrailingClosureArgument(unsigned paramIdx) const;

  /// The ParamDecl at the given index if the parameter has an applied
  /// property wrapper.
  bool hasExternalPropertyWrapper(unsigned paramIdx) const;

  /// Whether the given parameter is a closure that should allow capture of
  /// 'self' to be implicit, without requiring "self.".
  bool isImplicitSelfCapture(unsigned paramIdx) const;

  /// Whether the given parameter is a closure that should inherit the
  /// actor context from the context in which it was created.
  bool inheritsActorContext(unsigned paramIdx) const;

  /// Whether there is any contextual information set on this parameter list.
  bool anyContextualInfo() const;

  bool isVariadicGenericParameter(unsigned paramIdx) const;

  /// Retrieve the number of non-defaulted parameters.
  unsigned numNonDefaultedParameters() const {
    return defaultArguments.count();
  }

  /// Retrieve the number of parameters for which we have information.
  unsigned size() const { return defaultArguments.size(); }
};

/// Turn a param list into a symbolic and printable representation that does not
/// include the types, something like (: , b:, c:)
std::string getParamListAsString(ArrayRef<AnyFunctionType::Param> parameters);

/// Describes a generic function type.
///
/// A generic function type describes a function that is polymorphic with
/// respect to some set of generic parameters and the requirements placed
/// on those parameters and dependent member types thereof. The input and
/// output types of the generic function can be expressed in terms of those
/// generic parameters.
class GenericFunctionType final : public AnyFunctionType,
    public llvm::FoldingSetNode,
    private llvm::TrailingObjects<GenericFunctionType, AnyFunctionType::Param,
                                  Type> {
  friend TrailingObjects;
      
  GenericSignature Signature;

  size_t numTrailingObjects(OverloadToken<AnyFunctionType::Param>) const {
    return getNumParams();
  }
                                    
  size_t numTrailingObjects(OverloadToken<Type>) const {
    return hasGlobalActor() ? 1 : 0;
  }

  /// Construct a new generic function type.
  GenericFunctionType(GenericSignature sig,
                      ArrayRef<Param> params,
                      Type result,
                      Optional<ExtInfo> info,
                      const ASTContext *ctx,
                      RecursiveTypeProperties properties);

public:
  /// Create a new generic function type.
  static GenericFunctionType *get(GenericSignature sig,
                                  ArrayRef<Param> params,
                                  Type result,
                                  Optional<ExtInfo> info = None);

  // Retrieve the input parameters of this function type.
  ArrayRef<Param> getParams() const {
    return {getTrailingObjects<Param>(), getNumParams()};
  }

  Type getGlobalActor() const {
    if (!hasGlobalActor())
      return Type();
    return *getTrailingObjects<Type>();
  }

  /// Retrieve the generic signature of this function type.
  GenericSignature getGenericSignature() const {
    return Signature;
  }
  
  /// Retrieve the generic parameters of this polymorphic function type.
  TypeArrayView<GenericTypeParamType> getGenericParams() const;

  /// Retrieve the requirements of this polymorphic function type.
  ArrayRef<Requirement> getRequirements() const;
                              
  /// Substitute the given generic arguments into this generic
  /// function type and return the resulting non-generic type.
  FunctionType *substGenericArgs(SubstitutionMap subs,
                                 SubstOptions options = None);
  FunctionType *substGenericArgs(llvm::function_ref<Type(Type)> substFn) const;

  void Profile(llvm::FoldingSetNodeID &ID) {
    Optional<ExtInfo> info = None;
    if (hasExtInfo())
      info = getExtInfo();
    Profile(ID, getGenericSignature(), getParams(), getResult(), info);
  }
  static void Profile(llvm::FoldingSetNodeID &ID,
                      GenericSignature sig,
                      ArrayRef<Param> params,
                      Type result,
                      Optional<ExtInfo> info);

  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::GenericFunction;
  }
};

BEGIN_CAN_TYPE_WRAPPER(GenericFunctionType, AnyFunctionType)
  /// Create a new generic function type.
  static CanGenericFunctionType get(CanGenericSignature sig,
                                    CanParamArrayRef params,
                                    CanType result,
                                    Optional<ExtInfo> info = None) {
    // Knowing that the argument types are independently canonical is
    // not sufficient to guarantee that the function type will be canonical.
    auto fnType =
        GenericFunctionType::get(sig, params.getOriginalArray(), result, info);
    return cast<GenericFunctionType>(fnType->getCanonicalType());
  }

  CanFunctionType substGenericArgs(SubstitutionMap subs) const;

  CanGenericSignature getGenericSignature() const {
    return CanGenericSignature(getPointer()->getGenericSignature());
  }
  
  ArrayRef<CanTypeWrapper<GenericTypeParamType>> getGenericParams() const {
    return getGenericSignature().getGenericParams();
  }

  CanGenericFunctionType withExtInfo(ExtInfo info) const {
    return CanGenericFunctionType(
                    cast<GenericFunctionType>(getPointer()->withExtInfo(info)));
  }
END_CAN_TYPE_WRAPPER(GenericFunctionType, AnyFunctionType)

inline CanAnyFunctionType
CanAnyFunctionType::get(CanGenericSignature signature, CanParamArrayRef params,
                        CanType result, Optional<ExtInfo> extInfo) {
  if (signature) {
    return CanGenericFunctionType::get(signature, params, result, extInfo);
  } else {
    return CanFunctionType::get(params, result, extInfo);
  }
}

inline GenericSignature AnyFunctionType::getOptGenericSignature() const {
  if (auto genericFn = dyn_cast<GenericFunctionType>(this)) {
    return genericFn->getGenericSignature();
  } else {
    return nullptr;
  }
}

inline CanGenericSignature CanAnyFunctionType::getOptGenericSignature() const {
  if (auto genericFn = dyn_cast<GenericFunctionType>(*this)) {
    return genericFn.getGenericSignature();
  } else {
    return CanGenericSignature();
  }
}

/// Conventions for passing arguments as parameters.
enum class ParameterConvention : uint8_t {
  /// This argument is passed indirectly, i.e. by directly passing the address
  /// of an object in memory.  The callee is responsible for destroying the
  /// object.  The callee may assume that the address does not alias any valid
  /// object.
  Indirect_In,

  /// This argument is passed indirectly, i.e. by directly passing the address
  /// of an object in memory.  The callee may not modify and does not destroy
  /// the object.
  Indirect_In_Guaranteed,

  /// This argument is passed indirectly, i.e. by directly passing the address
  /// of an object in memory.  The object is always valid, but the callee may
  /// assume that the address does not alias any valid object and reorder loads
  /// stores to the parameter as long as the whole object remains valid. Invalid
  /// single-threaded aliasing may produce inconsistent results, but should
  /// remain memory safe.
  Indirect_Inout,

  /// This argument is passed indirectly, i.e. by directly passing the address
  /// of an object in memory. The object is allowed to be aliased by other
  /// well-typed references, but is not allowed to be escaped. This is the
  /// convention used by mutable captures in @noescape closures.
  Indirect_InoutAliasable,

  /// This argument is passed directly.  Its type is non-trivial, and the callee
  /// is responsible for destroying it.
  Direct_Owned,

  /// This argument is passed directly.  Its type may be trivial, or it may
  /// simply be that the callee is not responsible for destroying it.  Its
  /// validity is guaranteed only at the instant the call begins.
  Direct_Unowned,

  /// This argument is passed directly.  Its type is non-trivial, and the caller
  /// guarantees its validity for the entirety of the call.
  Direct_Guaranteed,

  /// This argument is a value pack of mutable references to storage,
  /// which the function is being given exclusive access to.  The elements
  /// must be passed indirectly.
  Pack_Inout,

  /// This argument is a value pack, and ownership of the elements is being
  /// transferred into this function.  Whether the elements are passed
  /// indirectly is recorded in the pack type.
  Pack_Owned,

  /// This argument is a value pack, and ownership of the elements is not
  /// being transferred into this function.  Whether the elements are passed
  /// indirectly is recorded in the pack type.
  Pack_Guaranteed,
};
// Check that the enum values fit inside Bits.SILFunctionType.
static_assert(unsigned(ParameterConvention::Pack_Guaranteed) < (1<<4),
              "fits in Bits.SILFunctionType");

// Does this parameter convention require indirect storage? This reflects a
// SILFunctionType's formal (immutable) conventions, as opposed to the transient
// SIL conventions that dictate SILValue types.
inline bool isIndirectFormalParameter(ParameterConvention conv) {
  switch (conv) {
  case ParameterConvention::Indirect_In:
  case ParameterConvention::Indirect_Inout:
  case ParameterConvention::Indirect_InoutAliasable:
  case ParameterConvention::Indirect_In_Guaranteed:
    return true;

  case ParameterConvention::Direct_Unowned:
  case ParameterConvention::Direct_Guaranteed:
  case ParameterConvention::Direct_Owned:
  case ParameterConvention::Pack_Inout:
  case ParameterConvention::Pack_Owned:
  case ParameterConvention::Pack_Guaranteed:
    return false;
  }
  llvm_unreachable("covered switch isn't covered?!");
}
inline bool isConsumedParameter(ParameterConvention conv) {
  switch (conv) {
  case ParameterConvention::Indirect_In:
  case ParameterConvention::Direct_Owned:
  case ParameterConvention::Pack_Owned:
    return true;

  case ParameterConvention::Indirect_Inout:
  case ParameterConvention::Indirect_InoutAliasable:
  case ParameterConvention::Direct_Unowned:
  case ParameterConvention::Direct_Guaranteed:
  case ParameterConvention::Indirect_In_Guaranteed:
  case ParameterConvention::Pack_Inout:
  case ParameterConvention::Pack_Guaranteed:
    return false;
  }
  llvm_unreachable("bad convention kind");
}

/// Returns true if conv is a guaranteed parameter. This may look unnecessary
/// but this will allow code to generalize to handle Indirect_Guaranteed
/// parameters when they are added.
inline bool isGuaranteedParameter(ParameterConvention conv) {
  switch (conv) {
  case ParameterConvention::Direct_Guaranteed:
  case ParameterConvention::Indirect_In_Guaranteed:
  case ParameterConvention::Pack_Guaranteed:
    return true;

  case ParameterConvention::Indirect_Inout:
  case ParameterConvention::Indirect_InoutAliasable:
  case ParameterConvention::Indirect_In:
  case ParameterConvention::Direct_Unowned:
  case ParameterConvention::Direct_Owned:
  case ParameterConvention::Pack_Inout:
  case ParameterConvention::Pack_Owned:
    return false;
  }
  llvm_unreachable("bad convention kind");
}

/// Returns true if conv indicates a pack parameter.
inline bool isPackParameter(ParameterConvention conv) {
  switch (conv) {
  case ParameterConvention::Pack_Guaranteed:
  case ParameterConvention::Pack_Inout:
  case ParameterConvention::Pack_Owned:
    return true;

  case ParameterConvention::Indirect_In_Guaranteed:
  case ParameterConvention::Indirect_Inout:
  case ParameterConvention::Indirect_InoutAliasable:
  case ParameterConvention::Indirect_In:
  case ParameterConvention::Direct_Guaranteed:
  case ParameterConvention::Direct_Unowned:
  case ParameterConvention::Direct_Owned:
    return false;
  }
  llvm_unreachable("bad convention kind");
}

/// The differentiability of a SIL function type parameter.
enum class SILParameterDifferentiability : bool {
  /// Either differentiable or not applicable.
  ///
  /// - If the function type is not `@differentiable`, parameter
  ///   differentiability is not applicable. This case is the default value.
  /// - If the function type is `@differentiable`, the function is
  ///   differentiable with respect to this parameter.
  DifferentiableOrNotApplicable,

  /// Not differentiable: a `@noDerivative` parameter.
  ///
  /// May be applied only to parameters of `@differentiable` function types.
  /// The function type is not differentiable with respect to this parameter.
  NotDifferentiable,
};

/// A parameter type and the rules for passing it.
class SILParameterInfo {
  CanType Type;
  ParameterConvention Convention;
  SILParameterDifferentiability Differentiability;

public:
  SILParameterInfo() = default;//: Ty(), Convention((ParameterConvention)0) {}
  SILParameterInfo(
      CanType type, ParameterConvention conv,
      SILParameterDifferentiability differentiability =
          SILParameterDifferentiability::DifferentiableOrNotApplicable)
      : Type(type), Convention(conv), Differentiability(differentiability) {
    assert(type->isLegalSILType() && "SILParameterInfo has illegal SIL type");
  }

  /// Return the unsubstituted parameter type that describes the abstract
  /// calling convention of the parameter.
  ///
  /// For most purposes, you probably want \c getArgumentType .
  CanType getInterfaceType() const {
    return Type;
  }
  
  /// Return the type of a call argument matching this parameter.
  ///
  /// \c t must refer back to the function type this is a parameter for.
  CanType getArgumentType(SILModule &M, const SILFunctionType *t, TypeExpansionContext context) const;
  ParameterConvention getConvention() const {
    return Convention;
  }
  // Does this parameter convention require indirect storage? This reflects a
  // SILFunctionType's formal (immutable) conventions, as opposed to the
  // transient SIL conventions that dictate SILValue types.
  bool isFormalIndirect() const {
    return isIndirectFormalParameter(getConvention());
  }

  bool isDirectGuaranteed() const {
    return getConvention() == ParameterConvention::Direct_Guaranteed;
  }

  bool isIndirectInGuaranteed() const {
    return getConvention() == ParameterConvention::Indirect_In_Guaranteed;
  }

  bool isIndirectIn() const {
    return getConvention() == ParameterConvention::Indirect_In;
  }

  bool isIndirectInOut() const {
    return getConvention() == ParameterConvention::Indirect_Inout;
  }
  bool isIndirectMutating() const {
    return getConvention() == ParameterConvention::Indirect_Inout
        || getConvention() == ParameterConvention::Indirect_InoutAliasable;
  }

  bool isPack() const {
    return isPackParameter(getConvention());
  }

  /// True if this parameter is consumed by the callee, either
  /// indirectly or directly.
  bool isConsumed() const {
    return isConsumedParameter(getConvention());
  }

  /// Returns true if this parameter is guaranteed, either indirectly or
  /// directly.
  bool isGuaranteed() const {
    return isGuaranteedParameter(getConvention());
  }

  SILParameterDifferentiability getDifferentiability() const {
    return Differentiability;
  }

  SILParameterInfo getWithDifferentiability(
      SILParameterDifferentiability differentiability) const {
    return SILParameterInfo(getInterfaceType(), getConvention(),
                            differentiability);
  }

  /// The SIL storage type determines the ABI for arguments based purely on the
  /// formal parameter conventions. The actual SIL type for the argument values
  /// may differ in canonical SIL. In particular, opaque values require indirect
  /// storage. Therefore they will be passed using an indirect formal
  /// convention, and this method will return an address type. However, in
  /// canonical SIL the opaque arguments might not have an address type.
  SILType getSILStorageType(
      SILModule &M, const SILFunctionType *t,
      TypeExpansionContext context) const; // in SILFunctionConventions.h
  SILType getSILStorageInterfaceType() const;

  /// Return a version of this parameter info with the type replaced.
  SILParameterInfo getWithInterfaceType(CanType type) const {
    return SILParameterInfo(type, getConvention(), getDifferentiability());
  }

  /// Return a version of this parameter info with the convention replaced.
  SILParameterInfo getWithConvention(ParameterConvention c) const {
    return SILParameterInfo(getInterfaceType(), c, getDifferentiability());
  }

  /// Transform this SILParameterInfo by applying the user-provided
  /// function to its type.
  ///
  /// Note that this does not perform a recursive transformation like
  /// Type::transform does.
  template<typename F>
  SILParameterInfo map(const F &fn) const {
    return getWithInterfaceType(fn(getInterfaceType()));
  }

  SILParameterInfo mapTypeOutOfContext() const {
    return getWithInterfaceType(getInterfaceType()->mapTypeOutOfContext()
                                                  ->getCanonicalType());
  }

  /// Treating this parameter info as a component of the given function
  /// type, apply any substitutions from the function type to it to
  /// get a substituted version of it, as you would get from
  /// SILFunctionType::getUnsubstitutedType.
  SILParameterInfo getUnsubstituted(SILModule &M, const SILFunctionType *fnType,
                                    TypeExpansionContext context) const {
    return getWithInterfaceType(getArgumentType(M, fnType, context));
  }

  void profile(llvm::FoldingSetNodeID &id) {
    id.AddPointer(getInterfaceType().getPointer());
    id.AddInteger((unsigned)getConvention());
    id.AddInteger((unsigned)getDifferentiability());
  }

  SWIFT_DEBUG_DUMP;
  void print(llvm::raw_ostream &out,
             const PrintOptions &options = PrintOptions()) const;
  void print(ASTPrinter &Printer, const PrintOptions &Options) const;
  friend llvm::raw_ostream &operator<<(llvm::raw_ostream &out,
                                       SILParameterInfo type) {
    type.print(out);
    return out;
  }

  bool operator==(SILParameterInfo rhs) const {
    return getInterfaceType() == rhs.getInterfaceType() &&
           getConvention() == rhs.getConvention() &&
           getDifferentiability() == rhs.getDifferentiability();
  }
  bool operator!=(SILParameterInfo rhs) const {
    return !(*this == rhs);
  }
};

/// Conventions for returning values.
enum class ResultConvention : uint8_t {
  /// This result is returned indirectly, i.e. by passing the address
  /// of an uninitialized object in memory.  The callee is responsible
  /// for leaving an initialized object at this address.  The callee
  /// may assume that the address does not alias any valid object.
  Indirect,

  /// The caller is responsible for destroying this return value.
  /// Its type is non-trivial.
  Owned,

  /// The caller is not responsible for destroying this return value.
  /// Its type may be trivial, or it may simply be offered unsafely.
  /// It is valid at the instant of the return, but further operations
  /// may invalidate it.
  Unowned,

  /// The caller is not responsible for destroying this return value.
  /// The validity of the return value is dependent on the 'self' parameter,
  /// so it may be invalidated if that parameter is released.
  UnownedInnerPointer,
  
  /// This value has been (or may have been) returned autoreleased.
  /// The caller should make an effort to reclaim the autorelease.
  /// The type must be a class or class existential type, and this
  /// must be the only return value.
  Autoreleased,

  /// This value is a pack that is returned indirectly by passing a
  /// pack address (which may or may not be further indirected,
  /// depending on the pact type).  The callee is responsible for
  /// leaving an initialized object in each element of the pack.
  Pack,
};

// Does this result require indirect storage for the purpose of reabstraction?
inline bool isIndirectFormalResult(ResultConvention convention) {
  return convention == ResultConvention::Indirect ||
         convention == ResultConvention::Pack;
}

/// The differentiability of a SIL function type result.
enum class SILResultDifferentiability : bool {
  /// Either differentiable or not applicable.
  ///
  /// - If the function type is not `@differentiable`, result
  ///   differentiability is not applicable. This case is the default value.
  /// - If the function type is `@differentiable`, the function is
  ///   differentiable with respect to this result.
  DifferentiableOrNotApplicable,

  /// Not differentiable: a `@noDerivative` result.
  ///
  /// May be applied only to result of `@differentiable` function types.
  /// The function type is not differentiable with respect to this result.
  NotDifferentiable,
};

/// A result type and the rules for returning it.
class SILResultInfo {
  CanType Type;
  ResultConvention Convention;
  SILResultDifferentiability Differentiability;

public:
  SILResultInfo() = default;
  SILResultInfo(CanType type, ResultConvention conv,
                SILResultDifferentiability differentiability =
                    SILResultDifferentiability::DifferentiableOrNotApplicable)
      : Type(type), Convention(conv), Differentiability(differentiability) {
    assert(type->isLegalSILType() && "SILResultInfo has illegal SIL type");
  }

  /// Return the unsubstituted parameter type that describes the abstract
  /// calling convention of the parameter.
  ///
  /// For most purposes, you probably want \c getReturnValueType .
  CanType getInterfaceType() const {
    return Type;
  }
  
  /// The type of a return value corresponding to this result.
  ///
  /// \c t must refer back to the function type this is a parameter for.
  CanType getReturnValueType(SILModule &M, const SILFunctionType *t,
                             TypeExpansionContext context) const;

  ResultConvention getConvention() const {
    return Convention;
  }

  SILResultDifferentiability getDifferentiability() const {
    return Differentiability;
  }

  SILResultInfo
  getWithDifferentiability(SILResultDifferentiability differentiability) const {
    return SILResultInfo(getInterfaceType(), getConvention(),
                         differentiability);
  }

  /// The SIL storage type determines the ABI for arguments based purely on the
  /// formal result conventions. The actual SIL type for the result values may
  /// differ in canonical SIL. In particular, opaque values require indirect
  /// storage. Therefore they will be returned using an indirect formal
  /// convention, and this method will return an address type. However, in
  /// canonical SIL the opaque results might not have an address type.
  SILType getSILStorageType(
      SILModule &M, const SILFunctionType *t,
      TypeExpansionContext context) const; // in SILFunctionConventions.h
  SILType getSILStorageInterfaceType() const;
  /// Return a version of this result info with the type replaced.
  SILResultInfo getWithInterfaceType(CanType type) const {
    return SILResultInfo(type, getConvention());
  }

  /// Return a version of this result info with the convention replaced.
  SILResultInfo getWithConvention(ResultConvention c) const {
    return SILResultInfo(getInterfaceType(), c);
  }

  // Does this result convention require indirect storage? This reflects a
  // SILFunctionType's formal (immutable) conventions, as opposed to the
  // transient SIL conventions that dictate SILValue types.
  bool isFormalIndirect() const {
    return isIndirectFormalResult(getConvention());
  }
  bool isFormalDirect() const {
    return !isIndirectFormalResult(getConvention());
  }

  /// Is this a pack result?  Pack results are always indirect.
  bool isPack() const {
    return getConvention() == ResultConvention::Pack;
  }

  /// Transform this SILResultInfo by applying the user-provided
  /// function to its type.
  ///
  /// Note that this does not perform a recursive transformation like
  /// Type::transform does.
  template <typename F>
  SILResultInfo map(F &&fn) const {
    return getWithInterfaceType(fn(getInterfaceType()));
  }

  SILResultInfo mapTypeOutOfContext() const {
    return getWithInterfaceType(getInterfaceType()->mapTypeOutOfContext()
                                                  ->getCanonicalType());
  }

  /// Treating this result info as a component of the given function
  /// type, apply any substitutions from the function type to it to
  /// get a substituted version of it, as you would get from
  /// SILFunctionType::getUnsubstitutedType.
  SILResultInfo getUnsubstituted(SILModule &M, const SILFunctionType *fnType,
                                 TypeExpansionContext context) const {
    return getWithInterfaceType(getReturnValueType(M, fnType, context));
  }

  void profile(llvm::FoldingSetNodeID &id) {
    id.AddPointer(Type.getPointer());
    id.AddInteger(unsigned(getConvention()));
    id.AddInteger(unsigned(getDifferentiability()));
  }

  SWIFT_DEBUG_DUMP;
  void print(llvm::raw_ostream &out,
             const PrintOptions &options = PrintOptions()) const;
  void print(ASTPrinter &Printer, const PrintOptions &Options) const;
  friend llvm::raw_ostream &operator<<(llvm::raw_ostream &out,
                                       SILResultInfo type) {
    type.print(out);
    return out;
  }

  ValueOwnershipKind
  getOwnershipKind(SILFunction &, CanSILFunctionType fTy) const; // in SILType.cpp

  bool operator==(SILResultInfo rhs) const {
    return Type == rhs.Type && Convention == rhs.Convention
      && Differentiability == rhs.Differentiability;
  }
  bool operator!=(SILResultInfo rhs) const {
    return !(*this == rhs);
  }
};

using YieldConvention = ParameterConvention;

/// The type and convention of a value yielded from a yield-once or
/// yield-many coroutine.
class SILYieldInfo : public SILParameterInfo {
public:
  SILYieldInfo() {}
  SILYieldInfo(CanType type, YieldConvention conv)
    : SILParameterInfo(type, conv) {
  }

  SILYieldInfo getWithInterfaceType(CanType type) const {
    return SILYieldInfo(type, getConvention());
  }

  /// Return a version of this yield info with the convention replaced.
  SILYieldInfo getWithConvention(YieldConvention c) const {
    return SILYieldInfo(getInterfaceType(), c);
  }

  template<typename F>
  SILYieldInfo map(const F &fn) const {
    return getWithInterfaceType(fn(getInterfaceType()));
  }

  SILYieldInfo mapTypeOutOfContext() const {
    return getWithInterfaceType(getInterfaceType()->mapTypeOutOfContext()
                                                  ->getCanonicalType());
  }

  CanType getYieldValueType(SILModule &M, const SILFunctionType *fnType,
                            TypeExpansionContext context) const {
    return getArgumentType(M, fnType, context);
  }

  /// Treating this yield info as a component of the given function
  /// type, apply any substitutions from the function type to it to
  /// get a substituted version of it, as you would get from
  /// SILFunctionType::getUnsubstitutedType.
  SILYieldInfo getUnsubstituted(SILModule &M, const SILFunctionType *fnType,
                                TypeExpansionContext context) const {
    return getWithInterfaceType(getYieldValueType(M, fnType, context));
  }
};

/// SILCoroutineKind - What kind of coroutine is this SILFunction?
enum class SILCoroutineKind : uint8_t {
  /// This function is not a coroutine.  It may have arbitrary normal
  /// results and may not have yield results.
  None,

  /// This function is a yield-once coroutine (used by e.g. accessors).
  /// It must not have normal results and may have arbitrary yield results.
  YieldOnce,

  /// This function is a yield-many coroutine (used by e.g. generators).
  /// It must not have normal results and may have arbitrary yield results.
  YieldMany,
};
  
class SILFunctionConventions;


CanType substOpaqueTypesWithUnderlyingTypes(CanType type,
                                            TypeExpansionContext context,
                                            bool allowLoweredTypes = false);
ProtocolConformanceRef
substOpaqueTypesWithUnderlyingTypes(ProtocolConformanceRef ref, Type origType,
                                    TypeExpansionContext context);
namespace Lowering {
  class TypeConverter;
}

/// SILFunctionType - The lowered type of a function value, suitable
/// for use by SIL.
///
/// This type is defined by the AST library because it must be capable
/// of appearing in secondary positions, e.g. within tuple and
/// function parameter and result types.
class SILFunctionType final
    : public TypeBase,
      public llvm::FoldingSetNode,
      private llvm::TrailingObjects<SILFunctionType, SILParameterInfo,
                                    SILResultInfo, SILYieldInfo,
                                    SubstitutionMap, CanType, ClangTypeInfo> {
  friend TrailingObjects;

  size_t numTrailingObjects(OverloadToken<SILParameterInfo>) const {
    return NumParameters;
  }

  size_t numTrailingObjects(OverloadToken<SILResultInfo>) const {
    return getNumResults() + (hasErrorResult() ? 1 : 0);
  }

  size_t numTrailingObjects(OverloadToken<SILYieldInfo>) const {
    return getNumYields();
  }

  size_t numTrailingObjects(OverloadToken<CanType>) const {
    return hasResultCache() ? 2 : 0;
  }

  size_t numTrailingObjects(OverloadToken<SubstitutionMap>) const {
    return size_t(hasPatternSubstitutions()) +
           size_t(hasInvocationSubstitutions());
  }

  size_t numTrailingObjects(OverloadToken<ClangTypeInfo>) const {
    return Bits.SILFunctionType.HasClangTypeInfo ? 1 : 0;
  }

public:
  using ExtInfo = SILExtInfo;
  using ExtInfoBuilder = SILExtInfoBuilder;
  using Language = SILExtInfoBuilder::Language;
  using Representation = SILExtInfoBuilder::Representation;

private:
  unsigned NumParameters;

  // These are *normal* results if this is not a coroutine and *yield* results
  // otherwise.
  unsigned NumAnyResults;         // Not including the ErrorResult.
  unsigned NumAnyIndirectFormalResults; // Subset of NumAnyResults.
  unsigned NumPackResults; // Subset of NumAnyIndirectFormalResults.

  // [NOTE: SILFunctionType-layout]
  // The layout of a SILFunctionType in memory is:
  //   SILFunctionType
  //   SILParameterInfo[NumParameters]
  //   SILResultInfo[isCoroutine() ? 0 : NumAnyResults]
  //   SILResultInfo?    // if hasErrorResult()
  //   SILYieldInfo[isCoroutine() ? NumAnyResults : 0]
  //   SubstitutionMap[HasPatternSubs + HasInvocationSubs]
  //   CanType?          // if !isCoro && NumAnyResults > 1, formal result cache
  //   CanType?          // if !isCoro && NumAnyResults > 1, all result cache

  CanGenericSignature InvocationGenericSig;
  ProtocolConformanceRef WitnessMethodConformance;

  MutableArrayRef<SILParameterInfo> getMutableParameters() {
    return {getTrailingObjects<SILParameterInfo>(), NumParameters};
  }

  MutableArrayRef<SILResultInfo> getMutableResults() {
    return {getTrailingObjects<SILResultInfo>(), getNumResults()};
  }

  MutableArrayRef<SILYieldInfo> getMutableYields() {
    return {getTrailingObjects<SILYieldInfo>(), getNumYields()};
  }

  SILResultInfo &getMutableErrorResult() {
    assert(hasErrorResult());
    return *(getTrailingObjects<SILResultInfo>() + getNumResults());
  }

  SubstitutionMap &getMutablePatternSubs() {
    assert(hasPatternSubstitutions());
    return *getTrailingObjects<SubstitutionMap>();
  }

  SubstitutionMap &getMutableInvocationSubs() {
    assert(hasInvocationSubstitutions());
    return *(getTrailingObjects<SubstitutionMap>()
               + unsigned(hasPatternSubstitutions()));
  }

  /// Do we have slots for caches of the normal-result tuple type?
  bool hasResultCache() const {
    return NumAnyResults > 1 && !isCoroutine();
  }

  CanType &getMutableFormalResultsCache() const {
    assert(hasResultCache());
    return *const_cast<SILFunctionType *>(this)->getTrailingObjects<CanType>();
  }

  CanType &getMutableAllResultsCache() const {
    assert(hasResultCache());
    return *(const_cast<SILFunctionType *>(this)->getTrailingObjects<CanType>()
             + 1);
  }

  SILFunctionType(GenericSignature genericSig, ExtInfo ext,
                  SILCoroutineKind coroutineKind,
                  ParameterConvention calleeConvention,
                  ArrayRef<SILParameterInfo> params,
                  ArrayRef<SILYieldInfo> yieldResults,
                  ArrayRef<SILResultInfo> normalResults,
                  Optional<SILResultInfo> errorResult,
                  SubstitutionMap patternSubs,
                  SubstitutionMap invocationSubs,
                  const ASTContext &ctx, RecursiveTypeProperties properties,
                  ProtocolConformanceRef witnessMethodConformance);

public:
  static CanSILFunctionType
  get(GenericSignature genericSig, ExtInfo ext, SILCoroutineKind coroutineKind,
      ParameterConvention calleeConvention,
      ArrayRef<SILParameterInfo> interfaceParams,
      ArrayRef<SILYieldInfo> interfaceYields,
      ArrayRef<SILResultInfo> interfaceResults,
      Optional<SILResultInfo> interfaceErrorResult,
      SubstitutionMap patternSubs, SubstitutionMap invocationSubs,
      const ASTContext &ctx,
      ProtocolConformanceRef witnessMethodConformance =
          ProtocolConformanceRef());

  /// Return a structurally-identical function type with a slightly tweaked
  /// ExtInfo.
  CanSILFunctionType getWithExtInfo(ExtInfo ext);

  /// Return a structurally-identical function type with a slightly tweaked
  /// representation.
  CanSILFunctionType getWithRepresentation(Representation repr);

  /// Given that this function type uses a C-language convention, return its
  /// formal semantic result type.
  ///
  /// C functions represented in SIL are always in one of three cases:
  ///   - no results at all; this corresponds to a void result type;
  ///   - a single direct result and no indirect results; or
  ///   - a single indirect result and no direct results.
  ///
  /// If the result is formally indirect, return the empty tuple.
  SILType getFormalCSemanticResult(SILModule &M);

  /// Return the convention under which the callee is passed, if this
  /// is a thick non-block callee.
  ParameterConvention getCalleeConvention() const {
    return ParameterConvention(Bits.SILFunctionType.CalleeConvention);
  }
  bool isCalleeConsumed() const {
    return getCalleeConvention() == ParameterConvention::Direct_Owned;
  }
  bool isCalleeUnowned() const {
    return getCalleeConvention() == ParameterConvention::Direct_Unowned;
  }
  bool isCalleeGuaranteed() const {
    return getCalleeConvention() == ParameterConvention::Direct_Guaranteed;
  }

  /// Is this some kind of coroutine?
  bool isCoroutine() const {
    return getCoroutineKind() != SILCoroutineKind::None;
  }
  SILCoroutineKind getCoroutineKind() const {
    return SILCoroutineKind(Bits.SILFunctionType.CoroutineKind);
  }

  bool isSendable() const { return getExtInfo().isSendable(); }
  bool isAsync() const { return getExtInfo().isAsync(); }

  /// Return the array of all the yields.
  ArrayRef<SILYieldInfo> getYields() const {
    return const_cast<SILFunctionType *>(this)->getMutableYields();
  }
  unsigned getNumYields() const { return isCoroutine() ? NumAnyResults : 0; }

  /// Return the array of all result information. This may contain inter-mingled
  /// direct and indirect results.
  ArrayRef<SILResultInfo> getResults() const {
    return const_cast<SILFunctionType *>(this)->getMutableResults();
  }
  unsigned getNumResults() const { return isCoroutine() ? 0 : NumAnyResults; }

  /// Given that this function type has exactly one result, return it.
  /// This is a common situation when working with a function with a known
  /// signature.  It is *not* safe to assume that C functions satisfy
  /// this, because void functions have zero results.
  SILResultInfo getSingleResult() const {
    assert(getNumResults() == 1);
    return getResults()[0];
  }

  /// Given that this function type has exactly one formally direct result,
  /// return it. Some formal calling conventions only apply when a single
  /// direct result is present.
  SILResultInfo getSingleDirectFormalResult() const {
    assert(getNumDirectFormalResults() == 1);
    for (auto &result : getResults()) {
      if (!result.isFormalIndirect())
        return result;
    }
    llvm_unreachable("Missing indirect result");
  }

  // Get the number of results that require a formal indirect calling
  // convention regardless of whether SIL requires address types. Even if the
  // substituted SIL types match, a formal direct argument may not be passed
  // to a formal indirect parameter and vice-versa. Hence, the formally
  // indirect property, not the SIL indirect property, should be consulted to
  // determine whether function reabstraction is necessary.
  unsigned getNumIndirectFormalResults() const {
    return isCoroutine() ? 0 : NumAnyIndirectFormalResults;
  }
  /// Does this function have any formally indirect results?
  bool hasIndirectFormalResults() const {
    return getNumIndirectFormalResults() != 0;
  }
  unsigned getNumDirectFormalResults() const {
    return isCoroutine() ? 0 : NumAnyResults - NumAnyIndirectFormalResults;
  }
  unsigned getNumPackResults() const {
    return isCoroutine() ? 0 : NumPackResults;
  }

  struct IndirectFormalResultFilter {
    bool operator()(SILResultInfo result) const {
      return result.isFormalIndirect();
    }
  };
  using IndirectFormalResultIter =
      llvm::filter_iterator<const SILResultInfo *, IndirectFormalResultFilter>;
  using IndirectFormalResultRange = iterator_range<IndirectFormalResultIter>;

  /// A range of SILResultInfo for all formally indirect results.
  IndirectFormalResultRange getIndirectFormalResults() const {
    return llvm::make_filter_range(getResults(), IndirectFormalResultFilter());
  }

  struct DirectFormalResultFilter {
    bool operator()(SILResultInfo result) const {
      return !result.isFormalIndirect();
    }
  };
  using DirectFormalResultIter =
      llvm::filter_iterator<const SILResultInfo *, DirectFormalResultFilter>;
  using DirectFormalResultRange = iterator_range<DirectFormalResultIter>;

  /// A range of SILResultInfo for all formally direct results.
  DirectFormalResultRange getDirectFormalResults() const {
    return llvm::make_filter_range(getResults(), DirectFormalResultFilter());
  }

  struct PackResultFilter {
    bool operator()(SILResultInfo result) const {
      return result.isPack();
    }
  };
  using PackResultIter =
      llvm::filter_iterator<const SILResultInfo *, PackResultFilter>;
  using PackResultRange = iterator_range<PackResultIter>;

  /// A range of SILResultInfo for all pack results.  Pack results are also
  /// included in the set of indirect results.
  PackResultRange getPackResults() const {
    return llvm::make_filter_range(getResults(), PackResultFilter());
  }

  /// Get a single non-address SILType that represents all formal direct
  /// results. The actual SIL result type of an apply instruction that calls
  /// this function depends on the current SIL stage and is known by
  /// SILFunctionConventions. It may be a wider tuple that includes formally
  /// indirect results.
  SILType getDirectFormalResultsType(SILModule &M,
                                     TypeExpansionContext expansion);

  unsigned getNumIndirectFormalYields() const {
    return isCoroutine() ? NumAnyIndirectFormalResults : 0;
  }
  /// Does this function have any formally indirect yields?
  bool hasIndirectFormalYields() const {
    return getNumIndirectFormalYields() != 0;
  }
  unsigned getNumDirectFormalYields() const {
    return isCoroutine() ? NumAnyResults - NumAnyIndirectFormalResults : 0;
  }
  unsigned getNumPackYields() const {
    return isCoroutine() ? NumPackResults : 0;
  }

  struct IndirectFormalYieldFilter {
    bool operator()(SILYieldInfo yield) const {
      return !yield.isFormalIndirect();
    }
  };

  using IndirectFormalYieldIter =
      llvm::filter_iterator<const SILYieldInfo *, IndirectFormalYieldFilter>;
  using IndirectFormalYieldRange = iterator_range<IndirectFormalYieldIter>;

  /// A range of SILYieldInfo for all formally Indirect Yields.
  IndirectFormalYieldRange getIndirectFormalYields() const {
    return llvm::make_filter_range(getYields(), IndirectFormalYieldFilter());
  }

  /// Get a single non-address SILType for all SIL results regardless of whether
  /// they are formally indirect. The actual SIL result type of an apply
  /// instruction that calls this function depends on the current SIL stage and
  /// is known by SILFunctionConventions. It may be a narrower tuple that omits
  /// formally indirect results.
  SILType getAllResultsSubstType(SILModule &M, TypeExpansionContext expansion);
  SILType getAllResultsInterfaceType();

  /// Does this function have a blessed Swift-native error result?
  bool hasErrorResult() const {
    return Bits.SILFunctionType.HasErrorResult;
  }
  SILResultInfo getErrorResult() const {
    return const_cast<SILFunctionType*>(this)->getMutableErrorResult();
  }
  Optional<SILResultInfo> getOptionalErrorResult() const {
    if (hasErrorResult()) {
      return getErrorResult();
    } else {
      return None;
    }
  }

  /// Returns the number of function parameters, not including any formally
  /// indirect results.
  unsigned getNumParameters() const { return NumParameters; }

  ArrayRef<SILParameterInfo> getParameters() const {
    return const_cast<SILFunctionType*>(this)->getMutableParameters();
  }

  /// Returns the 'self' parameter, assuming that this is the type of
  /// a method.
  SILParameterInfo getSelfParameter() const {
    return getParameters().back();
  }

  struct IndirectMutatingParameterFilter {
    bool operator()(SILParameterInfo param) const {
      return param.isIndirectMutating();
    }
  };
  using IndirectMutatingParameterIter =
      llvm::filter_iterator<const SILParameterInfo *,
                            IndirectMutatingParameterFilter>;
  using IndirectMutatingParameterRange =
      iterator_range<IndirectMutatingParameterIter>;

  /// A range of SILParameterInfo for all indirect mutating parameters.
  IndirectMutatingParameterRange getIndirectMutatingParameters() const {
    return llvm::make_filter_range(getParameters(),
                                   IndirectMutatingParameterFilter());
  }

  /// Returns the number of indirect mutating parameters.
  unsigned getNumIndirectMutatingParameters() const {
    return llvm::count_if(getParameters(), IndirectMutatingParameterFilter());
  }

  /// Get the generic signature that the component types are specified
  /// in terms of, if any.
  CanGenericSignature getSubstGenericSignature() const {
    if (hasPatternSubstitutions())
      return getPatternGenericSignature();
    return getInvocationGenericSignature();
  }

  /// Return the combined substitutions that need to be applied to the
  /// component types to render their expected types in the context.
  SubstitutionMap getCombinedSubstitutions() const {
    if (hasPatternSubstitutions()) {
      auto subs = getPatternSubstitutions();
      if (hasInvocationSubstitutions())
        subs = subs.subst(getInvocationSubstitutions());
      return subs;
    }
    return getInvocationSubstitutions();
  }

  /// Get the generic signature used by callers to invoke the function.
  CanGenericSignature getInvocationGenericSignature() const {
    return InvocationGenericSig;
  }

  bool hasInvocationSubstitutions() const {
    return Bits.SILFunctionType.HasInvocationSubs;
  }

  /// Return the invocation substitutions.  The presence of invocation
  /// substitutions means that this is an applied or contextualized generic
  /// function type.
  SubstitutionMap getInvocationSubstitutions() const {
    return hasInvocationSubstitutions()
             ? const_cast<SILFunctionType*>(this)->getMutableInvocationSubs()
             : SubstitutionMap();
  }

  bool hasPatternSubstitutions() const {
    return Bits.SILFunctionType.HasPatternSubs;
  }

  /// Return the generic signature which expresses the fine-grained
  /// abstraction of this function.  See the explanation for
  /// `getPatternSubstitutions`.
  ///
  /// The exact structure of this signature is an implementation detail
  /// for many function types, and tools must be careful not to expose
  /// it in ways that must be kept stable.  For example, ptrauth
  /// discrimination does not distinguish between different generic
  /// parameter types in this signature.
  CanGenericSignature getPatternGenericSignature() const  {
    return hasPatternSubstitutions()
             ? CanGenericSignature(
                 getPatternSubstitutions().getGenericSignature())
             : CanGenericSignature();
  }

  /// Return the "pattern" substitutions.  The presence of pattern
  /// substitutions means that the component types of this type are
  /// abstracted in some additional way.
  ///
  /// For example, in the function:
  ///
  /// ```
  ///   func consume<T>(producer: () -> T)
  /// ```
  ///
  /// the argument function `producer` is abstracted differently from
  /// a function of type `() -> Int`, even when `T` is concretely `Int`.
  ///
  /// Similarly, a protocol witness function that returns an `Int` might
  /// return it differently if original requirement is abstract about its
  /// return type.
  ///
  /// The most important abstraction differences are accounted for in
  /// the structure and conventions of the function's component types,
  /// but more subtle differences, like ptrauth discrimination, may
  /// require more precise information.
  SubstitutionMap getPatternSubstitutions() const {
    return hasPatternSubstitutions()
             ? const_cast<SILFunctionType*>(this)->getMutablePatternSubs()
             : SubstitutionMap();
  }

  bool isPolymorphic() const {
    return getInvocationGenericSignature() && !getInvocationSubstitutions();
  }

  CanType getSelfInstanceType(SILModule &M, TypeExpansionContext context) const;

  /// If this is a @convention(witness_method) function with a class
  /// constrained self parameter, return the class constraint for the
  /// Self type.
  ClassDecl *getWitnessMethodClass(SILModule &M,
                                   TypeExpansionContext context) const;

  /// If this is a @convention(witness_method) function, return the conformance
  /// for which the method is a witness. If it isn't that convention, return
  /// an invalid conformance.
  ProtocolConformanceRef getWitnessMethodConformanceOrInvalid() const {
    return WitnessMethodConformance;
  }

  ClangTypeInfo getClangTypeInfo() const;

  /// Returns true if the function type stores a Clang type that cannot
  /// be derived from its Swift type. Returns false otherwise, including if
  /// the function type is not @convention(c) or @convention(block).
  bool hasNonDerivableClangType();

  bool hasSameExtInfoAs(const SILFunctionType *otherFn);

  /// Given that `this` is a `@differentiable` function type, returns an
  /// `IndexSubset` corresponding to the differentiability parameters
  /// (e.g. all parameters except the `@noDerivative` ones).
  IndexSubset *getDifferentiabilityParameterIndices();

  /// Given that `this` is a `@differentiable` function type, returns an
  /// `IndexSubset` corresponding to the differentiability results
  /// (e.g. all results except the `@noDerivative` ones).
  IndexSubset *getDifferentiabilityResultIndices();

  /// Returns the `@differentiable` function type for the given
  /// differentiability kind and differentiability parameter/result indices.
  CanSILFunctionType getWithDifferentiability(DifferentiabilityKind kind,
                                              IndexSubset *parameterIndices,
                                              IndexSubset *resultIndices);

  /// Returns the SIL function type stripping differentiability kind and
  /// differentiability from all parameters.
  CanSILFunctionType getWithoutDifferentiability();

  /// Given that `this` is a `@differentiable` function type, returns the type
  /// of the given `@differentiable` function type component.
  CanSILFunctionType getDifferentiableComponentType(
      NormalDifferentiableFunctionTypeComponent component, SILModule &module);

  /// Given that `this` is a `@differentiable(linear)` function type, returns
  /// the type of the given `@differentiable(linear)` function type component.
  CanSILFunctionType
  getLinearComponentType(LinearDifferentiableFunctionTypeComponent component,
                         SILModule &module);

  /// Returns the type of the derivative function for the given parameter
  /// indices, result indices, derivative function kind, derivative function
  /// generic signature (optional), and other auxiliary parameters.
  ///
  /// Preconditions:
  /// - Parameters corresponding to parameter indices must conform to
  ///   `Differentiable`.
  /// - Results corresponding to result indices must conform to
  ///   `Differentiable`.
  ///
  /// Typing rules, given:
  /// - Original function type: $(T0, T1, ...) -> (R0, R1, ...)
  ///
  /// Terminology:
  /// - The derivative of a `Differentiable`-conforming type has the
  ///   `TangentVector` associated type. `TangentVector` is abbreviated as `Tan`
  ///   below.
  /// - "wrt" parameters refers to parameters indicated by the parameter
  ///   indices.
  /// - "wrt" result refers to the result indicated by the result index.
  ///
  /// JVP derivative type:
  /// - Takes original parameters.
  /// - Returns original results, followed by a differential function, which
  ///   takes "wrt" parameter derivatives and returns a "wrt" result derivative.
  ///
  /// \verbatim
  ///     $(T0, ...) -> (R0, ...,  (T0.Tan, T1.Tan, ...) -> R0.Tan)
  ///                    ^~~~~~~    ^~~~~~~~~~~~~~~~~~~     ^~~~~~
  ///          original results | derivatives wrt params | derivative wrt result
  /// \endverbatim
  ///
  /// VJP derivative type:
  /// - Takes original parameters.
  /// - Returns original results, followed by a pullback function, which
  ///   takes a "wrt" result derivative and returns "wrt" parameter derivatives.
  ///
  /// \verbatim
  ///     $(T0, ...) -> (R0, ...,       (R0.Tan)  ->     (T0.Tan, T1.Tan, ...))
  ///                    ^~~~~~~         ^~~~~~            ^~~~~~~~~~~~~~~~~~~
  ///          original results | derivative wrt result | derivatives wrt params
  /// \endverbatim
  ///
  /// The original type may have `inout` parameters. If so, the
  /// differential/pullback typing rules are more nuanced: see documentation for
  /// `getAutoDiffDerivativeFunctionLinearMapType` for details. Semantically,
  /// `inout` parameters behave as both parameters and results.
  ///
  /// A "constrained derivative generic signature" is computed from
  /// `derivativeFunctionGenericSignature`, if specified. Otherwise, it is
  /// computed from the original generic signature. A "constrained derivative
  /// generic signature" requires all "wrt" parameters to conform to
  /// `Differentiable`; this is important for correctness.
  ///
  /// This "constrained derivative generic signature" is used for
  /// parameter/result type lowering. It is used as the actual generic signature
  /// of the derivative function type iff the original function type has a
  /// generic signature and not all generic parameters are bound to concrete
  /// types. Otherwise, no derivative generic signature is used.
  ///
  /// Other properties of the original function type are copied exactly:
  /// `ExtInfo`, coroutine kind, callee convention, yields, optional error
  /// result, witness method conformance, etc.
  ///
  /// Special cases:
  /// - Reabstraction thunks have special derivative type calculation. The
  ///   original function-typed last parameter is transformed into a
  ///   `@differentiable` function-typed parameter in the derivative type. This
  ///   is necessary for the differentiation transform to support reabstraction
  ///   thunk differentiation because the function argument is opaque and cannot
  ///   be differentiated. Instead, the argument is made `@differentiable` and
  ///   reabstraction thunk JVP/VJP callers are responsible for passing a
  ///   `@differentiable` function.
  ///   - TODO(TF-1036): Investigate more efficient reabstraction thunk
  ///     derivative approaches. The last argument can simply be a
  ///     corresponding derivative function, instead of a `@differentiable`
  ///     function - this is more direct. It may be possible to implement
  ///     reabstraction thunk derivatives using "reabstraction thunks for
  ///     the original function's derivative", avoiding extra code generation.
  CanSILFunctionType getAutoDiffDerivativeFunctionType(
      IndexSubset *parameterIndices, IndexSubset *resultIndices,
      AutoDiffDerivativeFunctionKind kind, Lowering::TypeConverter &TC,
      LookupConformanceFn lookupConformance,
      CanGenericSignature derivativeFunctionGenericSignature = nullptr,
      bool isReabstractionThunk = false,
      CanType origTypeOfAbstraction = CanType());


  /// Returns the type of the transpose function for the given parameter
  /// indices, transpose function generic signature (optional), and other
  /// auxiliary parameters.
  ///
  /// Preconditions:
  /// - Linearity parameters corresponding to parameter indices must conform to
  ///   `Differentiable` and satisfy `Self == Self.TangentVector`.
  ///
  /// Typing rules, given:
  /// - Original function type: $(T0, T1, ...) -> (R0, R1, ...)
  ///
  /// Transpose function type:
  /// - Takes non-linearity parameters, followed by original results, as
  ///   parameters.
  /// - Returns linearity parameters.
  ///
  /// A "constrained transpose generic signature" is computed from
  /// `transposeFunctionGenericSignature`, if specified. Otherwise, it is
  /// computed from the original generic signature. A "constrained transpose
  /// generic signature" requires all linearity parameters to conform to
  /// `Differentiable` and to satisfy `Self == Self.TangentVector`; this is
  /// important for correctness.
  ///
  /// This "constrained transpose generic signature" is used for
  /// parameter/result type lowering. It is used as the actual generic signature
  /// of the transpose function type iff the original function type has a
  /// generic signature and not all generic parameters are bound to concrete
  /// types. Otherwise, no transpose generic signature is used.
  ///
  /// Other properties of the original function type are copied exactly:
  /// `ExtInfo`, callee convention, witness method conformance, etc.
  CanSILFunctionType getAutoDiffTransposeFunctionType(
      IndexSubset *parameterIndices, Lowering::TypeConverter &TC,
      LookupConformanceFn lookupConformance,
      CanGenericSignature transposeFunctionGenericSignature = nullptr);

  ExtInfo getExtInfo() const {
    return ExtInfo(Bits.SILFunctionType.ExtInfoBits, getClangTypeInfo());
  }

  /// Returns the language-level calling convention of the function.
  Language getLanguage() const {
    return getExtInfo().getLanguage();
  }

  bool hasSelfParam() const {
    return getExtInfo().hasSelfParam();
  }

  /// Get the representation of the function type.
  Representation getRepresentation() const {
    return getExtInfo().getRepresentation();
  }

  bool isPseudogeneric() const {
    return getExtInfo().isPseudogeneric();
  }

  bool isNoEscape() const {
    return getExtInfo().isNoEscape();
  }

  /// Thick swift noescape function types are trivial.
  bool isTrivialNoEscape() const {
    return isNoEscape() &&
           getRepresentation() == SILFunctionTypeRepresentation::Thick;
  }

  bool isDifferentiable() const { return getExtInfo().isDifferentiable(); }
  DifferentiabilityKind getDifferentiabilityKind() const {
    return getExtInfo().getDifferentiabilityKind();
  }

  bool isNoReturnFunction(SILModule &M, TypeExpansionContext context)
      const; // Defined in SILType.cpp

  /// Create a SILFunctionType with the same structure as this one,
  /// but with a different (or new) set of invocation substitutions.
  /// The substitutions must have the same generic signature as this.
  CanSILFunctionType
  withInvocationSubstitutions(SubstitutionMap subs) const;

  /// Create a SILFunctionType with the same structure as this one,
  /// but with a different set of pattern substitutions.
  /// This type must already have pattern substitutions, and they
  /// must have the same signature as the new substitutions.
  CanSILFunctionType
  withPatternSubstitutions(SubstitutionMap subs) const;

  /// Create a SILFunctionType with the same structure as this one,
  /// but replacing the invocation generic signature and pattern
  /// substitutions.  This type must either be polymorphic or have
  /// pattern substitutions, and the substitution signature must
  /// match `getSubstGenericSignature()`.
  CanSILFunctionType
  withPatternSpecialization(CanGenericSignature sign,
                            SubstitutionMap subs,
                            ProtocolConformanceRef witnessConformance =
                              ProtocolConformanceRef()) const;

  class ABICompatibilityCheckResult {
    friend class SILFunctionType;

    enum innerty {
      None,
      DifferentFunctionRepresentations,
      ABIEscapeToNoEscapeConversion,
      DifferentNumberOfResults,
      DifferentReturnValueConventions,
      ABIIncompatibleReturnValues,
      DifferentErrorResultConventions,
      ABIIncompatibleErrorResults,
      DifferentNumberOfParameters,
      DifferingParameterConvention,
      ABIIncompatibleParameterType,
    } kind;
    Optional<uintptr_t> payload;

    ABICompatibilityCheckResult(innerty kind) : kind(kind) {}
    ABICompatibilityCheckResult(innerty kind, uintptr_t payload)
        : kind(kind), payload(payload) {}

  public:
    ABICompatibilityCheckResult() = delete;

    bool isCompatible() const { return kind == innerty::None; }
    bool isCompatibleUpToNoEscapeConversion() {
      return kind == innerty::None ||
             kind == innerty::ABIEscapeToNoEscapeConversion;
    }

    bool hasPayload() const { return payload.has_value(); }
    uintptr_t getPayload() const { return payload.value(); }
    StringRef getMessage() const;
  };

  /// Returns no-error if this SILFunctionType is ABI compatible with \p
  /// other. Otherwise, it returns a true error with a message in
  /// std::error_code. This is only meant to be used in assertions. When
  /// assertions are disabled, this just returns true.
  ABICompatibilityCheckResult
  isABICompatibleWith(CanSILFunctionType other,
                      SILFunction &context) const;

  CanSILFunctionType substGenericArgs(SILModule &silModule,
                                      SubstitutionMap subs,
                                      TypeExpansionContext context);
  CanSILFunctionType substGenericArgs(SILModule &silModule,
                                      TypeSubstitutionFn subs,
                                      LookupConformanceFn conformances,
                                      TypeExpansionContext context);
  CanSILFunctionType substGenericArgs(SILModule &silModule,
                                      InFlightSubstitution &IFS,
                                      TypeExpansionContext context);
  CanSILFunctionType substituteOpaqueArchetypes(Lowering::TypeConverter &TC,
                                                TypeExpansionContext context);

  SILType substInterfaceType(SILModule &M,
                             SILType interfaceType,
                             TypeExpansionContext context) const;

  /// Return the unsubstituted function type equivalent to this type; that is, the type that has the same
  /// argument and result types as `this` type after substitutions, if any.
  CanSILFunctionType getUnsubstitutedType(SILModule &M) const;
                                    
  void Profile(llvm::FoldingSetNodeID &ID) {
    Profile(ID, getInvocationGenericSignature(),
            getExtInfo(), getCoroutineKind(), getCalleeConvention(),
            getParameters(), getYields(), getResults(),
            getOptionalErrorResult(), getWitnessMethodConformanceOrInvalid(),
            getPatternSubstitutions(), getInvocationSubstitutions());
  }
  static void
  Profile(llvm::FoldingSetNodeID &ID, GenericSignature genericSig, ExtInfo info,
          SILCoroutineKind coroutineKind, ParameterConvention calleeConvention,
          ArrayRef<SILParameterInfo> params, ArrayRef<SILYieldInfo> yields,
          ArrayRef<SILResultInfo> results, Optional<SILResultInfo> errorResult,
          ProtocolConformanceRef conformance,
          SubstitutionMap patternSub, SubstitutionMap invocationSubs);

  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::SILFunction;
  }
};
DEFINE_EMPTY_CAN_TYPE_WRAPPER(SILFunctionType, Type)

class SILBoxType;
class SILLayout; // From SIL
class SILModule; // From SIL
typedef CanTypeWrapper<SILBoxType> CanSILBoxType;

/// The SIL-only type for boxes, which represent a reference to a (non-class)
/// refcounted value referencing an aggregate with a given lowered layout.
class SILBoxType final : public TypeBase, public llvm::FoldingSetNode
{
  SILLayout *Layout;
  SubstitutionMap Substitutions;

  SILBoxType(ASTContext &C,
             SILLayout *Layout, SubstitutionMap Substitutions);

public:
  static CanSILBoxType get(ASTContext &C,
                           SILLayout *Layout,
                           SubstitutionMap Substitutions);

  SILLayout *getLayout() const { return Layout; }
  SubstitutionMap getSubstitutions() const { return Substitutions; }

  // TODO: SILBoxTypes should be explicitly constructed in terms of specific
  // layouts. As a staging mechanism, we expose the old single-boxed-type
  // interface.
  
  static CanSILBoxType get(CanType BoxedType);

  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::SILBox;
  }
  
  /// Produce a profile of this box, for use in a folding set.
  static void Profile(llvm::FoldingSetNodeID &id,
                      SILLayout *Layout,
                      SubstitutionMap Args);
  
  /// Produce a profile of this box, for use in a folding set.
  void Profile(llvm::FoldingSetNodeID &id) {
    Profile(id, getLayout(), getSubstitutions());
  }
};
DEFINE_EMPTY_CAN_TYPE_WRAPPER(SILBoxType, Type)

class SILMoveOnlyWrappedType;
class SILModule; // From SIL
typedef CanTypeWrapper<SILMoveOnlyWrappedType> CanSILMoveOnlyWrappedType;

/// A wrapper type that marks an inner type as being a move only value. Can not
/// be written directly at the Swift level, instead it is triggered by adding
/// the type attribute @_moveOnly to a different type. We transform these in
/// TypeLowering into a moveOnly SILType on the inner type.
class SILMoveOnlyWrappedType final : public TypeBase,
                                     public llvm::FoldingSetNode {
  CanType innerType;

  SILMoveOnlyWrappedType(CanType innerType)
      : TypeBase(TypeKind::SILMoveOnlyWrapped, &innerType->getASTContext(),
                 innerType->getRecursiveProperties()),
        innerType(innerType) {}

public:
  CanType getInnerType() const { return innerType; }

  static CanSILMoveOnlyWrappedType get(CanType innerType);

  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::SILMoveOnlyWrapped;
  }
};
DEFINE_EMPTY_CAN_TYPE_WRAPPER(SILMoveOnlyWrappedType, Type)

class SILBlockStorageType;
typedef CanTypeWrapper<SILBlockStorageType> CanSILBlockStorageType;
  
/// The SIL-only type @block_storage T, which represents the layout of an
/// on-stack block that captures a value of type T.
///
/// This type does not have to be able to appear positionally, unlike
/// SILFunctionType, so it is only parsed and defined within the SIL library.
class SILBlockStorageType : public TypeBase {
  CanType CaptureType;
  
  SILBlockStorageType(CanType CaptureType)
    : TypeBase(TypeKind::SILBlockStorage,
               &CaptureType->getASTContext(),
               CaptureType->getRecursiveProperties()),
      CaptureType(CaptureType) {}
  
public:
  static CanSILBlockStorageType get(CanType CaptureType);
                      
  CanType getCaptureType() const { return CaptureType; }
  // In SILType.h
  SILType getCaptureAddressType() const;
  
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::SILBlockStorage;
  }
};
DEFINE_EMPTY_CAN_TYPE_WRAPPER(SILBlockStorageType, Type)

/// A singleton 'token' type, which establishes a formal dependency
/// between two SIL nodes.  A token 'value' cannot be abstracted in
/// SIL: it cannot be returned, yielded, or passed as a function or
/// block argument.
class SILTokenType final : public TypeBase {
  friend class ASTContext;
  SILTokenType(const ASTContext &C)
    : TypeBase(TypeKind::SILToken, &C, RecursiveTypeProperties()) {}
public:
  // The singleton instance of this type is ASTContext::TheSILTokenType.

  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::SILToken;
  }
};
DEFINE_EMPTY_CAN_TYPE_WRAPPER(SILTokenType, Type)

/// A lowered pack type which structurally carries lowered information
/// about the pack elements.
///
/// A value pack is basically treated as a unique-ownership, unmovable
/// reference-semantics aggregate in SIL: ownership of the pack as a whole
/// is communicated with normal borrows of the pack, and packs can only
/// be created locally and forwarded as arguments rather than being moved
/// in any more complex way.
class SILPackType final : public TypeBase, public llvm::FoldingSetNode,
    private llvm::TrailingObjects<SILPackType, CanType> {
public:
  /// Type structure not reflected in the pack element type list.
  ///
  /// In the design of this, we considered storing ownership here,
  /// but ended up just with the one bit.
  struct ExtInfo {
    bool ElementIsAddress;

    ExtInfo(bool elementIsAddress) : ElementIsAddress(elementIsAddress) {}
  };

private:
  friend TrailingObjects;
  friend class ASTContext;
  SILPackType(const ASTContext &ctx, RecursiveTypeProperties properties,
              ExtInfo info, ArrayRef<CanType> elements)
      : TypeBase(TypeKind::SILPack, &ctx, properties) {
    Bits.SILPackType.Count = elements.size();
    Bits.SILPackType.ElementIsAddress = info.ElementIsAddress;
    memcpy(getTrailingObjects<CanType>(), elements.data(),
           elements.size() * sizeof(CanType));
  }

public:
  static CanTypeWrapper<SILPackType> get(const ASTContext &ctx,
                                         ExtInfo info,
                                         ArrayRef<CanType> elements);

  ExtInfo getExtInfo() const {
    return { isElementAddress() };
  }

  bool isElementAddress() const {
    return Bits.SILPackType.ElementIsAddress;
  }

  /// Retrieves the number of elements in this pack.
  unsigned getNumElements() const { return Bits.SILPackType.Count; }

  /// Retrieves the type of the elements in the pack.
  ArrayRef<CanType> getElementTypes() const {
    return {getTrailingObjects<CanType>(), getNumElements()};
  }

  /// Returns the type of the element at the given \p index.
  /// This is a lowered SIL type.
  CanType getElementType(unsigned index) const {
    return getTrailingObjects<CanType>()[index];
  }

  SILType getSILElementType(unsigned index) const; // in SILType.h

  /// Return the reduced shape of this pack.  For consistency with
  /// general shape-handling routines, we produce an AST pack type
  /// as the shape, not a SIL pack type.
  CanTypeWrapper<PackType> getReducedShape() const;

  /// Construct a formal pack type with the same shape as this
  /// lowered pack type, making a best effort to use the same element
  /// types.
  CanTypeWrapper<PackType> getApproximateFormalPackType() const;

  bool containsPackExpansionType() const;

  void Profile(llvm::FoldingSetNodeID &ID) const {
    Profile(ID, getExtInfo(), getElementTypes());
  }
  static void Profile(llvm::FoldingSetNodeID &ID,
                      ExtInfo info,
                      ArrayRef<CanType> elements);

  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::SILPack;
  }
};
BEGIN_CAN_TYPE_WRAPPER(SILPackType, Type)
  CanType getElementType(unsigned elementNo) const {
    return getPointer()->getElementType(elementNo);
  }

  ArrayRef<CanType> getElementTypes() const {
    return getPointer()->getElementTypes();
  }
END_CAN_TYPE_WRAPPER(SILPackType, Type)

/// A type with a special syntax that is always sugar for a library type. The
/// library type may have multiple base types. For unary syntax sugar, see
/// UnarySyntaxSugarType.
///
/// The prime examples are:
/// Arrays: [T] -> Array<T>
/// Optionals: T? -> Optional<T>
/// Dictionaries: [K : V]  -> Dictionary<K, V>
class SyntaxSugarType : public SugarType {
protected:
  // Syntax sugar types are never canonical.
  SyntaxSugarType(TypeKind K, const ASTContext &ctx,
                  RecursiveTypeProperties properties)
    : SugarType(K, &ctx, properties) {}

public:
  Type getImplementationType() const { return getSinglyDesugaredType(); }

  static bool classof(const TypeBase *T) {
    return T->getKind() >= TypeKind::First_SyntaxSugarType &&
           T->getKind() <= TypeKind::Last_SyntaxSugarType;
  }
};

/// A type with a special syntax that is always sugar for a library type that
/// wraps a single other type.
///
/// The prime examples are arrays ([T] -> Array<T>) and
/// optionals (T? -> Optional<T>).
class UnarySyntaxSugarType : public SyntaxSugarType {
  Type Base;

protected:
  UnarySyntaxSugarType(TypeKind K, const ASTContext &ctx, Type base,
                       RecursiveTypeProperties properties)
    : SyntaxSugarType(K, ctx, properties), Base(base) {}

public:
  Type getBaseType() const {
    return Base;
  }

  static bool classof(const TypeBase *T) {
    return T->getKind() >= TypeKind::First_UnarySyntaxSugarType &&
           T->getKind() <= TypeKind::Last_UnarySyntaxSugarType;
  }
};
  
/// The type [T], which is always sugar for a library type.
class ArraySliceType : public UnarySyntaxSugarType {
  ArraySliceType(const ASTContext &ctx, Type base,
                 RecursiveTypeProperties properties)
    : UnarySyntaxSugarType(TypeKind::ArraySlice, ctx, base, properties) {}

public:
  /// Return a uniqued array slice type with the specified base type.
  static ArraySliceType *get(Type baseTy);

  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::ArraySlice;
  }
};

/// The type T?, which is always sugar for a library type.
class OptionalType : public UnarySyntaxSugarType {
  OptionalType(const ASTContext &ctx,Type base,
               RecursiveTypeProperties properties)
    : UnarySyntaxSugarType(TypeKind::Optional, ctx, base, properties) {}

public:
  /// Return a uniqued optional type with the specified base type.
  static OptionalType *get(Type baseTy);

  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::Optional;
  }
};

/// The dictionary type [K : V], which is syntactic sugar for Dictionary<K, V>.
///
/// Example:
/// \code
/// var dict: [String : Int] = ["hello" : 0, "world" : 1]
/// \endcode
class DictionaryType : public SyntaxSugarType {
  Type Key;
  Type Value;

protected:
  // Syntax sugar types are never canonical.
  DictionaryType(const ASTContext &ctx, Type key, Type value,
                 RecursiveTypeProperties properties)
    : SyntaxSugarType(TypeKind::Dictionary, ctx, properties), 
      Key(key), Value(value) {}

public:
  /// Return a uniqued dictionary type with the specified key and value types.
  static DictionaryType *get(Type keyTy, Type valueTy);

  Type getKeyType() const { return Key; }
  Type getValueType() const { return Value; }

  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::Dictionary;
  }

  static bool classof(const DictionaryType *T) {
    return true;
  }
};

/// The type T..., which is sugar for a sequence of argument values.
class VariadicSequenceType : public UnarySyntaxSugarType {
  VariadicSequenceType(const ASTContext &ctx, Type base,
                       RecursiveTypeProperties properties)
    : UnarySyntaxSugarType(TypeKind::VariadicSequence, ctx, base, properties) {}

public:
  /// Return a uniqued variadic sequence type with the specified base type.
  static VariadicSequenceType *get(Type baseTy);

  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::VariadicSequence;
  }
};

/// ProtocolType - A protocol type describes an abstract interface implemented
/// by another type.
class ProtocolType : public NominalType {
public:
  /// Retrieve the type when we're referencing the given protocol.
  /// declaration.
  static ProtocolType *get(ProtocolDecl *D, Type Parent, const ASTContext &C);

  ProtocolDecl *getDecl() const {
    return reinterpret_cast<ProtocolDecl *>(NominalType::getDecl());
  }

  /// True if only classes may conform to the protocol.
  bool requiresClass();

  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::Protocol;
  }

  /// Canonicalizes the given set of protocols by eliminating any mentions
  /// of protocols that are already covered by inheritance due to other entries
  /// in the protocol list, then sorting them in some stable order.
  static void canonicalizeProtocols(SmallVectorImpl<ProtocolDecl *> &protocols);

  /// Visit all of the protocols in the given list of protocols, along with their
  ///
  /// \param fn Visitor function called for each protocol (just once). If it
  /// returns \c true, the visit operation will abort and return \c true.
  ///
  /// \returns \c true if any invocation of \c fn returns \c true, and \c false
  /// otherwise.
  static bool visitAllProtocols(ArrayRef<ProtocolDecl *> protocols,
                                llvm::function_ref<bool(ProtocolDecl *)> fn);

private:
  friend class NominalTypeDecl;
  ProtocolType(ProtocolDecl *TheDecl, Type Parent, const ASTContext &Ctx,
               RecursiveTypeProperties properties);
};
BEGIN_CAN_TYPE_WRAPPER(ProtocolType, NominalType)
END_CAN_TYPE_WRAPPER(ProtocolType, NominalType)

/// ProtocolCompositionType - A type that composes some number of protocols
/// together to represent types that conform to all of the named protocols.
///
/// \code
/// protocol P { /* ... */ }
/// protocol Q { /* ... */ }
/// var x : P & Q
/// \endcode
///
/// Here, the type of x is a composition of the protocols 'P' and 'Q'.
///
/// The canonical form of a protocol composition type is based on a sorted (by
/// module and name), minimized (based on redundancy due to protocol
/// inheritance) protocol list. If the sorted, minimized list is a single
/// protocol, then the canonical type is that protocol type. Otherwise, it is
/// a composition of the protocols in that list.
class ProtocolCompositionType final : public TypeBase,
    public llvm::FoldingSetNode,
    private llvm::TrailingObjects<ProtocolCompositionType, Type> {
  friend TrailingObjects;
  
public:
  /// Retrieve an instance of a protocol composition type with the
  /// given set of members.
  static Type get(const ASTContext &C, ArrayRef<Type> Members,
                  bool HasExplicitAnyObject);

  /// Canonical protocol composition types are minimized only to a certain
  /// degree to preserve ABI compatibility. This routine enables performing
  /// slower, but stricter minimization at need (e.g. redeclaration checking).
  CanType getMinimalCanonicalType(const DeclContext *useDC) const;

  /// Retrieve the set of members composed to create this type.
  ///
  /// For non-canonical types, this can contain classes, protocols and
  /// protocol compositions in any order. There can be at most one unique
  /// class constraint, either stated directly or as recursive member.
  ///
  /// In canonical types, this list will contain the superclass first if
  /// any, followed by zero or more protocols in a canonical sorted order,
  /// minimized to remove duplicates or protocols implied by inheritance.
  ///
  /// Note that the list of members is not sufficient to uniquely identify
  /// a protocol composition type; you also have to look at
  /// hasExplicitAnyObject().
  ArrayRef<Type> getMembers() const {
    return {getTrailingObjects<Type>(), Bits.ProtocolCompositionType.Count};
  }

  void Profile(llvm::FoldingSetNodeID &ID) {
    Profile(ID, getMembers(), hasExplicitAnyObject());
  }
  static void Profile(llvm::FoldingSetNodeID &ID,
                      ArrayRef<Type> Members,
                      bool HasExplicitAnyObject);

  /// True if the composition requires the concrete conforming type to
  /// be a class, either via a directly-stated superclass constraint or
  /// one of its member protocols being class-constrained.
  bool requiresClass();

  /// True if the class requirement is stated directly via '& AnyObject'.
  bool hasExplicitAnyObject() const {
    return Bits.ProtocolCompositionType.HasExplicitAnyObject;
  }

  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::ProtocolComposition;
  }
  
private:
  static ProtocolCompositionType *build(const ASTContext &C,
                                        ArrayRef<Type> Members,
                                        bool HasExplicitAnyObject);

  ProtocolCompositionType(const ASTContext *ctx, ArrayRef<Type> members,
                          bool hasExplicitAnyObject,
                          RecursiveTypeProperties properties)
    : TypeBase(TypeKind::ProtocolComposition, /*Context=*/ctx, properties) {
    Bits.ProtocolCompositionType.HasExplicitAnyObject = hasExplicitAnyObject;
    Bits.ProtocolCompositionType.Count = members.size();
    std::uninitialized_copy(members.begin(), members.end(),
                            getTrailingObjects<Type>());
  }
};
BEGIN_CAN_TYPE_WRAPPER(ProtocolCompositionType, Type)
  CanTypeArrayRef getMembers() const {
    return CanTypeArrayRef(getPointer()->getMembers());
  }
END_CAN_TYPE_WRAPPER(ProtocolCompositionType, Type)

/// ParameterizedProtocolType - A type that constrains one or more primary
/// associated type of a protocol to a list of argument types.
///
/// Written like a bound generic type, eg Sequence<Int>.
///
/// For now, these are only supported in generic requirement-like contexts:
/// - Inheritance clauses of protocols, generic parameters, associated types
/// - Conformance requirements in where clauses
/// - Extensions
/// - Opaque result types
///
/// Assuming that the primary associated type of Sequence is Element, the
/// desugaring is that T : Sequence<Int> is equivalent to
///
/// \code
/// T : Sequence where T.Element == Int.
/// \endcode
class ParameterizedProtocolType final : public TypeBase,
    public llvm::FoldingSetNode,
    private llvm::TrailingObjects<ParameterizedProtocolType, Type> {
  friend struct ExistentialLayout;
  friend TrailingObjects;

  ProtocolType *Base;

public:
  /// Retrieve an instance of a protocol composition type with the
  /// given set of members.
  static ParameterizedProtocolType *get(const ASTContext &C, ProtocolType *base,
                                        ArrayRef<Type> args);

  ProtocolType *getBaseType() const {
    return Base;
  }

  ProtocolDecl *getProtocol() const {
    return Base->getDecl();
  }

  ArrayRef<Type> getArgs() const {
    return {getTrailingObjects<Type>(),
            Bits.ParameterizedProtocolType.ArgCount};
  }

  bool requiresClass() const {
    return getBaseType()->requiresClass();
  }

  void getRequirements(Type baseType, SmallVectorImpl<Requirement> &reqs) const;

  void Profile(llvm::FoldingSetNodeID &ID) {
    Profile(ID, Base, getArgs());
  }
  static void Profile(llvm::FoldingSetNodeID &ID,
                      ProtocolType *base,
                      ArrayRef<Type> args);

  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::ParameterizedProtocol;
  }

private:
  ParameterizedProtocolType(const ASTContext *ctx,
                            ProtocolType *base,
                            ArrayRef<Type> args,
                            RecursiveTypeProperties properties);
};
BEGIN_CAN_TYPE_WRAPPER(ParameterizedProtocolType, Type)
  static CanParameterizedProtocolType get(const ASTContext &C,
                                          ProtocolType *base,
                                          ArrayRef<Type> args) {
    return CanParameterizedProtocolType(
        ParameterizedProtocolType::get(C, base, args));
  }

  CanProtocolType getBaseType() const {
    return CanProtocolType(getPointer()->getBaseType());
  }
  CanTypeArrayRef getArgs() const {
    return CanTypeArrayRef(getPointer()->getArgs());
  }
END_CAN_TYPE_WRAPPER(ParameterizedProtocolType, Type)

/// The generalized shape of an existential type.
struct ExistentialTypeGeneralization {
  /// The generalized existential type.  May refer to type parameters
  /// from the generalization signature.
  Type Shape;

  /// The generalization signature and substitutions.
  SubstitutionMap Generalization;

  /// Retrieve the generalization for the given existential type.
  ///
  /// Substituting the generalization substitutions into the shape
  /// should produce the original existential type.
  ///
  /// If there is a generic type substitution which can turn existential
  /// type A into existential type B, then:
  /// - the generalized shape types of A and B must be equal and
  /// - the generic signatures of the generalization substitutions of
  ///   A and B must be equal.
  static ExistentialTypeGeneralization get(Type existentialType);
};

/// An existential type, spelled with \c any .
///
/// In Swift 5 mode, a plain protocol name in type
/// context is an implicit existential type.
class ExistentialType final : public TypeBase {
  Type ConstraintType;

  /// Whether to print this existential type with the 'any' keyword,
  /// e.g. in diagnostics.
  ///
  /// Any and AnyObject need not use 'any', and they are printed
  /// in diagnostics without 'any' unless wrapped in MetatypeType.
  /// This field should only be used by TypePrinter.
  bool PrintWithAny;

  ExistentialType(Type constraintType,
                  bool printWithAny,
                  const ASTContext *canonicalContext,
                  RecursiveTypeProperties properties)
      : TypeBase(TypeKind::Existential, canonicalContext, properties),
        ConstraintType(constraintType), PrintWithAny(printWithAny) {}

public:
  static Type get(Type constraint);

  Type getConstraintType() const { return ConstraintType; }

  bool shouldPrintWithAny() const { return PrintWithAny; }

  void forcePrintWithAny(llvm::function_ref<void(Type)> print) {
    bool oldValue = PrintWithAny;
    PrintWithAny = true;
    print(this);
    PrintWithAny = oldValue;
  }

  bool requiresClass() const {
    if (auto protocol = ConstraintType->getAs<ProtocolType>())
      return protocol->requiresClass();

    if (auto composition = ConstraintType->getAs<ProtocolCompositionType>())
      return composition->requiresClass();

    if (auto paramProtocol = ConstraintType->getAs<ParameterizedProtocolType>())
      return paramProtocol->requiresClass();

    return false;
  }

  static bool classof(const TypeBase *type) {
    return type->getKind() == TypeKind::Existential;
  }
};
BEGIN_CAN_TYPE_WRAPPER(ExistentialType, Type)
  static CanExistentialType get(CanType constraint);
  PROXY_CAN_TYPE_SIMPLE_GETTER(getConstraintType)
END_CAN_TYPE_WRAPPER(ExistentialType, Type)


/// BuiltinTupleType - A singleton nominal type which serves as the declared
/// interface type of Builtin.TheTupleType.
class BuiltinTupleType : public NominalType {
public:
  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::BuiltinTuple;
  }

private:
  friend class ASTContext;
  BuiltinTupleType(BuiltinTupleDecl *TheDecl, const ASTContext &Ctx);
};
BEGIN_CAN_TYPE_WRAPPER(BuiltinTupleType, NominalType)
END_CAN_TYPE_WRAPPER(BuiltinTupleType, NominalType)

/// LValueType - An l-value is a handle to a physical object.  The
/// type of that object uniquely determines the type of an l-value
/// for it.
///
/// L-values are not fully first-class in Swift:
///
///  A type is said to "carry" an l-value if
///   - it is an l-value type or
///   - it is a tuple and at least one of its element types
///     carries an l-value.
///
/// The type of a function argument may carry an l-value.  This is done by
/// annotating the bound variable with InOutType.
///
/// The type of a return value, local variable, or field may not
/// carry an l-value.
///
/// When inferring a value type from an expression whose type
/// carries an l-value, the carried l-value types are converted
/// to their object type.
class LValueType : public TypeBase {
  Type ObjectTy;

  LValueType(Type objectTy, const ASTContext *canonicalContext,
             RecursiveTypeProperties properties)
    : TypeBase(TypeKind::LValue, canonicalContext, properties),
      ObjectTy(objectTy) {}

public:
  static LValueType *get(Type type);

  Type getObjectType() const { return ObjectTy; }

  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *type) {
    return type->getKind() == TypeKind::LValue;
  }
};
BEGIN_CAN_TYPE_WRAPPER(LValueType, Type)
  PROXY_CAN_TYPE_SIMPLE_GETTER(getObjectType)
  static CanLValueType get(CanType type) {
    return CanLValueType(LValueType::get(type));
  }
END_CAN_TYPE_WRAPPER(LValueType, Type)
  
/// InOutType - An inout qualified type is an argument to a function passed
/// with an explicit "Address of" operator.  It is read in and then written back
/// to after the callee function is done.  This also models the receiver of
/// @mutable methods on value types.
///
class InOutType : public TypeBase {
  Type ObjectTy;
  
  InOutType(Type objectTy, const ASTContext *canonicalContext,
            RecursiveTypeProperties properties)
  : TypeBase(TypeKind::InOut, canonicalContext, properties),
    ObjectTy(objectTy) {}
  
public:
  static InOutType *get(Type type);
  
  Type getObjectType() const { return ObjectTy; }
  
  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *type) {
    return type->getKind() == TypeKind::InOut;
  }
};
BEGIN_CAN_TYPE_WRAPPER(InOutType, Type)
PROXY_CAN_TYPE_SIMPLE_GETTER(getObjectType)
static CanInOutType get(CanType type) {
  return CanInOutType(InOutType::get(type));
}
END_CAN_TYPE_WRAPPER(InOutType, Type)


/// SubstitutableType - A reference to a type that can be substituted, i.e.,
/// an archetype or a generic parameter.
class SubstitutableType : public TypeBase {
protected:
  SubstitutableType(TypeKind K, const ASTContext *C,
                    RecursiveTypeProperties properties)
    : TypeBase(K, C, properties) { }

public:
  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() >= TypeKind::First_SubstitutableType &&
           T->getKind() <= TypeKind::Last_SubstitutableType;
  }
};
DEFINE_EMPTY_CAN_TYPE_WRAPPER(SubstitutableType, Type)

/// Common trailing objects for all ArchetypeType implementations, used to
/// store the constraints on the archetype.
template<typename Base, typename...AdditionalTrailingObjects>
using ArchetypeTrailingObjects = llvm::TrailingObjects<Base,
  ProtocolDecl *, Type, LayoutConstraint, AdditionalTrailingObjects...>;

class PrimaryArchetypeType;
class OpaqueTypeArchetypeType;
  
/// An archetype is a type that represents a runtime type that is
/// known to conform to some set of requirements.
///
/// Archetypes are used to represent generic type parameters and their
/// associated types, as well as the runtime type stored within an
/// existential container.
class ArchetypeType : public SubstitutableType,
                private llvm::trailing_objects_internal::TrailingObjectsBase
{
protected:
  // Each subclass has these same trailing objects and flags.
  size_t numTrailingObjects(OverloadToken<ProtocolDecl *>) const {
    return Bits.ArchetypeType.NumProtocols;
  }

  size_t numTrailingObjects(OverloadToken<Type>) const {
    return Bits.ArchetypeType.HasSuperclass ? 1 : 0;
  }

  size_t numTrailingObjects(OverloadToken<LayoutConstraint>) const {
    return Bits.ArchetypeType.HasLayoutConstraint ? 1 : 0;
  }
  Type InterfaceType;
  GenericEnvironment *Environment = nullptr;

  // Helper to get the trailing objects of one of the subclasses.
  template<typename Type>
  const Type *getSubclassTrailingObjects() const;
  
  template<typename Type>
  Type *getSubclassTrailingObjects() {
    const auto *constThis = this;
    return const_cast<Type*>(constThis->getSubclassTrailingObjects<Type>());
  }

public:
  /// Retrieve the name of this archetype.
  Identifier getName() const;

  /// Retrieve the fully-dotted name that should be used to display this
  /// archetype.
  std::string getFullName() const;

  /// Retrieve the interface type of this associated type, which will either
  /// be a GenericTypeParamType or a DependentMemberType.
  Type getInterfaceType() const { return InterfaceType; }

  /// getConformsTo - Retrieve the set of protocols to which this substitutable
  /// type shall conform.
  ArrayRef<ProtocolDecl *> getConformsTo() const {
    return { getSubclassTrailingObjects<ProtocolDecl *>(),
             static_cast<size_t>(Bits.ArchetypeType.NumProtocols) };
  }
  
  /// requiresClass - True if the type can only be substituted with class types.
  /// This is true if the type conforms to one or more class protocols or has
  /// a superclass constraint.
  bool requiresClass() const;

  /// Retrieve the superclass of this type, if such a requirement exists.
  Type getSuperclass() const {
    if (!Bits.ArchetypeType.HasSuperclass) return Type();

    return *getSubclassTrailingObjects<Type>();
  }

  /// Retrieve the layout constraint of this type, if such a requirement exists.
  LayoutConstraint getLayoutConstraint() const {
    if (!Bits.ArchetypeType.HasLayoutConstraint) return LayoutConstraint();

    return *getSubclassTrailingObjects<LayoutConstraint>();
  }

  /// Return true if the archetype has any requirements at all.
  bool hasRequirements() const {
    return !getConformsTo().empty() || getSuperclass();
  }

  /// Retrieve the nested type with the given associated type.
  Type getNestedType(AssociatedTypeDecl *assocType);

  /// Retrieve the nested type with the given name.
  ///
  /// This is a slow operation because it must scan all of the protocols to
  /// which the archetype conforms.
  Type getNestedTypeByName(Identifier name);

  /// Retrieve the parent of this archetype, or null if this is a
  /// primary archetype.
  ArchetypeType *getParent() const;

  /// Return the archetype that represents the root generic parameter of its
  /// interface type.
  ArchetypeType *getRoot() const;

  /// Determine whether this is a root archetype within the environment.
  bool isRoot() const;

  /// Get the generic environment this archetype lives in.
  GenericEnvironment *getGenericEnvironment() const { return Environment; }
  
  /// Get the protocol/class existential type that most closely represents the
  /// set of constraints on this archetype.
  ///
  /// Right now, this only considers constraints on the archetype itself, not
  /// any of its associated types, since those are the only kind of existential
  /// type we can represent.
  Type getExistentialType() const;

  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() >= TypeKind::First_ArchetypeType
        && T->getKind() <= TypeKind::Last_ArchetypeType;
  }
protected:
  ArchetypeType(TypeKind Kind,
                const ASTContext &C,
                RecursiveTypeProperties properties,
                Type InterfaceType,
                ArrayRef<ProtocolDecl *> ConformsTo,
                Type Superclass, LayoutConstraint Layout,
                GenericEnvironment *Environment);
};
BEGIN_CAN_TYPE_WRAPPER(ArchetypeType, SubstitutableType)
END_CAN_TYPE_WRAPPER(ArchetypeType, SubstitutableType)
  
/// An archetype that represents a primary generic argument inside the generic
/// context that binds it.
class PrimaryArchetypeType final : public ArchetypeType,
    private ArchetypeTrailingObjects<PrimaryArchetypeType>
{
  friend TrailingObjects;
  friend ArchetypeType;
                                  
public:
  /// getNew - Create a new primary archetype with the given name.
  ///
  /// The ConformsTo array will be minimized then copied into the ASTContext
  /// by this routine.
  static CanTypeWrapper<PrimaryArchetypeType>
                        getNew(const ASTContext &Ctx,
                               GenericEnvironment *GenericEnv,
                               Type InterfaceType,
                               SmallVectorImpl<ProtocolDecl *> &ConformsTo,
                               Type Superclass, LayoutConstraint Layout);

  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::PrimaryArchetype;
  }
private:
  PrimaryArchetypeType(const ASTContext &Ctx,
                       GenericEnvironment *GenericEnv,
                       Type InterfaceType,
                       ArrayRef<ProtocolDecl *> ConformsTo,
                       Type Superclass, LayoutConstraint Layout);
};
BEGIN_CAN_TYPE_WRAPPER(PrimaryArchetypeType, ArchetypeType)
END_CAN_TYPE_WRAPPER(PrimaryArchetypeType, ArchetypeType)

/// An archetype that represents an opaque type.
class OpaqueTypeArchetypeType final : public ArchetypeType,
    private ArchetypeTrailingObjects<OpaqueTypeArchetypeType>
{
  friend TrailingObjects;
  friend ArchetypeType;
  friend GenericSignatureBuilder;

  friend class GenericEnvironment;

  static OpaqueTypeArchetypeType *getNew(
      GenericEnvironment *environment, Type interfaceType,
      ArrayRef<ProtocolDecl*> conformsTo, Type superclass,
      LayoutConstraint layout);

public:
  /// Get an opaque archetype representing the underlying type of the given
  /// opaque type decl's interface type. For example, in
  /// `(some P, some Q)`, `some P`'s interface type would be the generic
  /// parameter with index 0, and `some Q`'s interface type would be the
  /// generic parameter with index 1.
  static Type get(OpaqueTypeDecl *decl, Type interfaceType,
                  SubstitutionMap subs);

  /// Retrieve the opaque type declaration.
  OpaqueTypeDecl *getDecl() const;

  /// Retrieve the set of substitutions applied to the opaque type.
  SubstitutionMap getSubstitutions() const;

  /// Compute the canonical interface type within the environment of this
  /// opaque type archetype.
  CanType getCanonicalInterfaceType(Type interfaceType);

  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::OpaqueTypeArchetype;
  }

private:
  OpaqueTypeArchetypeType(GenericEnvironment *environment,
                          RecursiveTypeProperties properties,
                          Type interfaceType,
                          ArrayRef<ProtocolDecl*> conformsTo,
                          Type superclass, LayoutConstraint layout);
};
BEGIN_CAN_TYPE_WRAPPER(OpaqueTypeArchetypeType, ArchetypeType)
END_CAN_TYPE_WRAPPER(OpaqueTypeArchetypeType, ArchetypeType)

enum class OpaqueSubstitutionKind {
  // Don't substitute the opaque type for the underlying type.
  DontSubstitute,
  // Substitute without looking at the type and context.
  // Can be done because the underlying type is from a minimally resilient
  // function (therefore must not contain private or internal types).
  AlwaysSubstitute,
  // Substitute in the same module into a maximal resilient context.
  // Can be done if the underlying type is accessible from the context we
  // substitute into. Private types cannot be accessed from a different TU.
  SubstituteSameModuleMaximalResilience,
  // Substitute in a different module from the opaque defining decl. Can only
  // be done if the underlying type is public.
  SubstituteNonResilientModule
};

/// A function object that can be used as a \c TypeSubstitutionFn and
/// \c LookupConformanceFn for \c Type::subst style APIs to map opaque
/// archetypes with underlying types visible at a given resilience expansion
/// to their underlying types.
class ReplaceOpaqueTypesWithUnderlyingTypes {
public:
  using SeenDecl = std::pair<OpaqueTypeDecl *, SubstitutionMap>;
private:
  ResilienceExpansion contextExpansion;
  llvm::PointerIntPair<const DeclContext *, 1, bool> inContextAndIsWholeModule;
  llvm::DenseSet<SeenDecl> *seenDecls;

public:
  ReplaceOpaqueTypesWithUnderlyingTypes(const DeclContext *inContext,
                                        ResilienceExpansion contextExpansion,
                                        bool isWholeModuleContext)
      : contextExpansion(contextExpansion),
        inContextAndIsWholeModule(inContext, isWholeModuleContext),
        seenDecls(nullptr) {}

  ReplaceOpaqueTypesWithUnderlyingTypes(
      const DeclContext *inContext, ResilienceExpansion contextExpansion,
      bool isWholeModuleContext, llvm::DenseSet<SeenDecl> &seen);

  /// TypeSubstitutionFn
  Type operator()(SubstitutableType *maybeOpaqueType) const;

  /// LookupConformanceFn
  ProtocolConformanceRef operator()(CanType maybeOpaqueType,
                                    Type replacementType,
                                    ProtocolDecl *protocol) const;

  OpaqueSubstitutionKind
  shouldPerformSubstitution(OpaqueTypeDecl *opaque) const;

  static OpaqueSubstitutionKind
  shouldPerformSubstitution(OpaqueTypeDecl *opaque, ModuleDecl *contextModule,
                            ResilienceExpansion contextExpansion);

private:
  const DeclContext *getContext() const {
    return inContextAndIsWholeModule.getPointer();
  }

  bool isWholeModule() const { return inContextAndIsWholeModule.getInt(); }
};

/// An archetype that's only valid in a portion of a local context.
class LocalArchetypeType : public ArchetypeType {
protected:
  using ArchetypeType::ArchetypeType;

public:
  LocalArchetypeType *getRoot() const {
    return cast<LocalArchetypeType>(ArchetypeType::getRoot());
  }

  static bool classof(const TypeBase *type) {
    return type->getKind() == TypeKind::OpenedArchetype ||
           type->getKind() == TypeKind::ElementArchetype;
  }
};
BEGIN_CAN_TYPE_WRAPPER(LocalArchetypeType, ArchetypeType)
  CanLocalArchetypeType getRoot() const {
    return CanLocalArchetypeType(getPointer()->getRoot());
  }
END_CAN_TYPE_WRAPPER(LocalArchetypeType, ArchetypeType)

/// An archetype that represents the dynamic type of an opened existential.
class OpenedArchetypeType final : public LocalArchetypeType,
    private ArchetypeTrailingObjects<OpenedArchetypeType>
{
  friend TrailingObjects;
  friend ArchetypeType;
  friend GenericEnvironment;

  UUID ID;

  /// Create a new opened archetype in the given environment representing
  /// the interface type.
  ///
  /// This is only invoked by the generic environment when mapping the
  /// interface type into context.
  static CanTypeWrapper<OpenedArchetypeType>
  getNew(GenericEnvironment *environment, Type interfaceType,
         ArrayRef<ProtocolDecl *> conformsTo, Type superclass,
         LayoutConstraint layout);

public:
  /// Compute the parameter that serves as the \c Self type for an opened
  /// archetype from the given outer generic signature.
  ///
  /// This type is a generic parameter one level deeper
  /// than the deepest generic context depth.
  static Type getSelfInterfaceTypeFromContext(GenericSignature parentSig,
                                              ASTContext &ctx);

public:
  /// Get or create an archetype that represents the opened type
  /// of an existential value.
  ///
  /// \param existential The existential type to open.
  /// \param parentSig The generic signature of the context opening
  /// this existential.
  ///
  /// \param knownID When non-empty, the known ID of the archetype. When empty,
  /// a fresh archetype with a unique ID will be opened.
  static CanTypeWrapper<OpenedArchetypeType> get(CanType existential,
                                                 GenericSignature parentSig,
                                                 Optional<UUID> knownID = None);

  /// Get or create an archetype that represents the opened type
  /// of an existential value.
  ///
  /// \param existential The existential type to open.
  /// \param interfaceType The interface type represented by this archetype.
  /// \param parentSig The generic signature of the context opening
  /// this existential.
  ///
  /// \param knownID When non-empty, the known ID of the archetype. When empty,
  /// a fresh archetype with a unique ID will be opened.
  static CanTypeWrapper<OpenedArchetypeType> get(CanType existential,
                                                 Type interfaceType,
                                                 GenericSignature parentSig,
                                                 Optional<UUID> knownID = None);

  /// Create a new archetype that represents the opened type
  /// of an existential value.
  ///
  /// Use this function when you are unsure of whether the
  /// \c existential type is a metatype or an instance type. This function
  /// will unwrap any existential metatype containers.
  ///
  /// \param existential The existential type or existential metatype to open.
  /// \param interfaceType The interface type represented by this archetype.
  /// \param parentSig The generic signature of the context opening
  /// this existential.
  static CanType getAny(CanType existential, Type interfaceType,
                        GenericSignature parentSig);

  /// Create a new archetype that represents the opened type
  /// of an existential value.
  ///
  /// Use this function when you are unsure of whether the
  /// \c existential type is a metatype or an instance type. This function
  /// will unwrap any existential metatype containers.
  ///
  /// \param existential The existential type or existential metatype to open.
  /// \param parentSig The generic signature of the context opening
  /// this existential.
  static CanType getAny(CanType existential, GenericSignature parentSig);

  /// Retrieve the ID number of this opened existential.
  UUID getOpenedExistentialID() const;

  /// Return the archetype that represents the root generic parameter of its
  /// interface type.
  OpenedArchetypeType *getRoot() const {
    return cast<OpenedArchetypeType>(ArchetypeType::getRoot());
  }

  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::OpenedArchetype;
  }
  
private:
  OpenedArchetypeType(GenericEnvironment *environment, Type interfaceType,
                      ArrayRef<ProtocolDecl *> conformsTo, Type superclass,
                      LayoutConstraint layout);
};
BEGIN_CAN_TYPE_WRAPPER(OpenedArchetypeType, LocalArchetypeType)
  CanOpenedArchetypeType getRoot() const {
    return CanOpenedArchetypeType(getPointer()->getRoot());
  }
END_CAN_TYPE_WRAPPER(OpenedArchetypeType, LocalArchetypeType)

/// A wrapper around a shape type to use in ArchetypeTrailingObjects
/// for PackArchetypeType.
struct PackShape {
  Type shapeType;
};

/// An archetype that represents an opaque element of a type
/// parameter pack in context.
class PackArchetypeType final
    : public ArchetypeType,
      private ArchetypeTrailingObjects<PackArchetypeType, PackShape> {
  friend TrailingObjects;
  friend ArchetypeType;

public:
  /// getNew - Create a new sequence archetype with the given name.
  ///
  /// The ConformsTo array will be minimized then copied into the ASTContext
  /// by this routine.
  static CanTypeWrapper<PackArchetypeType>
  get(const ASTContext &Ctx, GenericEnvironment *GenericEnv,
      Type InterfaceType, Type ShapeType,
      SmallVectorImpl<ProtocolDecl *> &ConformsTo, Type Superclass,
      LayoutConstraint Layout);

  // Returns the reduced shape type for this pack archetype.
  CanType getReducedShape() const;

  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::PackArchetype;
  }

private:
  PackArchetypeType(const ASTContext &Ctx, GenericEnvironment *GenericEnv,
                    Type InterfaceType, ArrayRef<ProtocolDecl *> ConformsTo,
                    Type Superclass, LayoutConstraint Layout, PackShape Shape);
};
BEGIN_CAN_TYPE_WRAPPER(PackArchetypeType, ArchetypeType)
END_CAN_TYPE_WRAPPER(PackArchetypeType, ArchetypeType)

/// An archetype that represents the element type of a pack archetype.
class ElementArchetypeType final : public LocalArchetypeType,
    private ArchetypeTrailingObjects<ElementArchetypeType>
{
  friend TrailingObjects;
  friend ArchetypeType;
  friend GenericEnvironment;

  UUID ID;

  /// Create a new element archetype in the given environment representing
  /// the interface type.
  ///
  /// This is only invoked by the generic environment when mapping the
  /// interface type into context.
  static CanTypeWrapper<ElementArchetypeType>
  getNew(GenericEnvironment *environment, Type interfaceType,
         ArrayRef<ProtocolDecl *> conformsTo, Type superclass,
         LayoutConstraint layout);

public:
  /// Retrieve the ID number of this opened element.
  UUID getOpenedElementID() const;

  /// Return the archetype that represents the root generic parameter of its
  /// interface type.
  ElementArchetypeType *getRoot() const {
    return cast<ElementArchetypeType>(ArchetypeType::getRoot());
  }

  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::ElementArchetype;
  }
  
private:
  ElementArchetypeType(const ASTContext &ctx,
                       GenericEnvironment *environment, Type interfaceType,
                       ArrayRef<ProtocolDecl *> conformsTo, Type superclass,
                       LayoutConstraint layout);
};
BEGIN_CAN_TYPE_WRAPPER(ElementArchetypeType, LocalArchetypeType)
  CanElementArchetypeType getRoot() const {
    return CanElementArchetypeType(getPointer()->getRoot());
  }
END_CAN_TYPE_WRAPPER(ElementArchetypeType, LocalArchetypeType)

template<typename Type>
const Type *ArchetypeType::getSubclassTrailingObjects() const {
  if (auto contextTy = dyn_cast<PrimaryArchetypeType>(this)) {
    return contextTy->getTrailingObjects<Type>();
  }
  if (auto opaqueTy = dyn_cast<OpaqueTypeArchetypeType>(this)) {
    return opaqueTy->getTrailingObjects<Type>();
  }
  if (auto openedTy = dyn_cast<OpenedArchetypeType>(this)) {
    return openedTy->getTrailingObjects<Type>();
  }
  if (auto childTy = dyn_cast<PackArchetypeType>(this)) {
    return childTy->getTrailingObjects<Type>();
  }
  if (auto childTy = dyn_cast<ElementArchetypeType>(this)) {
    return childTy->getTrailingObjects<Type>();
  }
  llvm_unreachable("unhandled ArchetypeType subclass?");
}
  
/// Describes the type of a generic parameter.
///
/// \sa GenericTypeParamDecl
class GenericTypeParamType : public SubstitutableType {
  static constexpr unsigned TYPE_SEQUENCE_BIT = (1 << 30);

  using DepthIndexTy = llvm::PointerEmbeddedInt<unsigned, 31>;

  /// The generic type parameter or depth/index.
  llvm::PointerUnion<GenericTypeParamDecl *, DepthIndexTy> ParamOrDepthIndex;

public:
  /// Retrieve a generic type parameter at the given depth and index.
  static GenericTypeParamType *get(bool isParameterPack, unsigned depth,
                                   unsigned index, const ASTContext &ctx);

  /// Retrieve the declaration of the generic type parameter, or null if
  /// there is no such declaration.
  GenericTypeParamDecl *getDecl() const {
    return ParamOrDepthIndex.dyn_cast<GenericTypeParamDecl *>();
  }

  /// Get the name of the generic type parameter.
  Identifier getName() const;
  
  /// The depth of this generic type parameter, i.e., the number of outer
  /// levels of generic parameter lists that enclose this type parameter.
  ///
  /// \code
  /// struct X<T> {
  ///   func f<U>() { }
  /// }
  /// \endcode
  ///
  /// Here 'T' has depth 0 and 'U' has depth 1. Both have index 0.
  unsigned getDepth() const;

  /// The index of this generic type parameter within its generic parameter
  /// list.
  ///
  /// \code
  /// struct X<T, U> {
  ///   func f<V>() { }
  /// }
  /// \endcode
  ///
  /// Here 'T' and 'U' have indexes 0 and 1, respectively. 'V' has index 0.
  unsigned getIndex() const;

  /// Returns \c true if this type parameter is declared as a pack.
  ///
  /// \code
  /// func foo<T...>() { }
  /// struct Foo<T...> { }
  /// \endcode
  bool isParameterPack() const;

  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::GenericTypeParam;
  }

private:
  friend class GenericTypeParamDecl;

  explicit GenericTypeParamType(GenericTypeParamDecl *param,
                                RecursiveTypeProperties props)
    : SubstitutableType(TypeKind::GenericTypeParam, nullptr, props),
      ParamOrDepthIndex(param) { }

  explicit GenericTypeParamType(bool isParameterPack, unsigned depth,
                                unsigned index, RecursiveTypeProperties props,
                                const ASTContext &ctx)
      : SubstitutableType(TypeKind::GenericTypeParam, &ctx, props),
        ParamOrDepthIndex(depth << 16 | index |
                          ((isParameterPack ? 1 : 0) << 30)) {}
};
BEGIN_CAN_TYPE_WRAPPER(GenericTypeParamType, SubstitutableType)
static CanGenericTypeParamType get(bool isParameterPack, unsigned depth,
                                   unsigned index, const ASTContext &C) {
  return CanGenericTypeParamType(
      GenericTypeParamType::get(isParameterPack, depth, index, C));
}
END_CAN_TYPE_WRAPPER(GenericTypeParamType, SubstitutableType)

/// A type that refers to a member type of some type that is dependent on a
/// generic parameter.
class DependentMemberType : public TypeBase {
  Type Base;
  llvm::PointerUnion<Identifier, AssociatedTypeDecl *> NameOrAssocType;

  DependentMemberType(Type base, Identifier name, const ASTContext *ctx,
                      RecursiveTypeProperties properties)
    : TypeBase(TypeKind::DependentMember, ctx, properties),
      Base(base), NameOrAssocType(name) { }

  DependentMemberType(Type base, AssociatedTypeDecl *assocType,
                      const ASTContext *ctx,
                      RecursiveTypeProperties properties)
    : TypeBase(TypeKind::DependentMember, ctx, properties),
      Base(base), NameOrAssocType(assocType) { }

public:
  static DependentMemberType *get(Type base, Identifier name);
  static DependentMemberType *get(Type base, AssociatedTypeDecl *assocType);

  /// Retrieve the base type.
  Type getBase() const { return Base; }

  /// Retrieve the name of the member type.
  Identifier getName() const;

  /// Retrieve the associated type referenced as a member.
  ///
  /// The associated type will only be available after successful type checking.
  AssociatedTypeDecl *getAssocType() const {
    return NameOrAssocType.dyn_cast<AssociatedTypeDecl *>();
  }
  
  /// Substitute the base type, looking up our associated type in it if it is
  /// non-dependent. Returns null if the member could not be found in the new
  /// base.
  Type substBaseType(ModuleDecl *M, Type base);

  /// Substitute the base type, looking up our associated type in it if it is
  /// non-dependent. Returns null if the member could not be found in the new
  /// base.
  Type substBaseType(Type base, LookupConformanceFn lookupConformance);

  /// Substitute the root generic type, looking up the chain of associated types.
  /// Returns null if the member could not be found in the new root.
  Type substRootParam(Type newRoot, LookupConformanceFn lookupConformance);

  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::DependentMember;
  }
};
BEGIN_CAN_TYPE_WRAPPER(DependentMemberType, Type)
  static CanDependentMemberType get(CanType base, AssociatedTypeDecl *assocType) {
    return CanDependentMemberType(DependentMemberType::get(base, assocType));
  }

  PROXY_CAN_TYPE_SIMPLE_GETTER(getBase)
END_CAN_TYPE_WRAPPER(DependentMemberType, Type)

/// The storage type of a variable with non-strong reference
/// ownership semantics.
///
/// The referent type always satisfies allowsOwnership().
///
/// These types may appear in the AST only as the type of a variable;
/// getTypeOfReference strips this layer from the formal type of a
/// reference to the variable.  However, it is extremely useful to
/// represent this as a distinct type in SIL and IR-generation.
class ReferenceStorageType : public TypeBase {
protected:
  ReferenceStorageType(TypeKind kind, Type referent, const ASTContext *C,
                       RecursiveTypeProperties properties)
    : TypeBase(kind, C, properties), Referent(referent) {}

private:
  Type Referent;
public:
  static ReferenceStorageType *get(Type referent, ReferenceOwnership ownership,
                                   const ASTContext &C);

  Type getReferentType() const { return Referent; }
  ReferenceOwnership getOwnership() const {
    switch (getKind()) {
#define REF_STORAGE(Name, ...) \
    case TypeKind::Name##Storage: \
      return ReferenceOwnership::Name;
#include "swift/AST/ReferenceStorage.def"
    default:
      llvm_unreachable("Unhandled reference storage type");
    }
  }

  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() >= TypeKind::First_ReferenceStorageType &&
           T->getKind() <= TypeKind::Last_ReferenceStorageType;
  }
};
BEGIN_CAN_TYPE_WRAPPER(ReferenceStorageType, Type)
static CanReferenceStorageType get(CanType referent,
                                   ReferenceOwnership ownership) {
  return CanReferenceStorageType(ReferenceStorageType::get(
      referent, ownership, referent->getASTContext()));
  }
  PROXY_CAN_TYPE_SIMPLE_GETTER(getReferentType)
END_CAN_TYPE_WRAPPER(ReferenceStorageType, Type)

#define REF_STORAGE_HELPER(Name, isLoadable) \
class Name##StorageType : public ReferenceStorageType { \
  friend class ReferenceStorageType; \
  Name##StorageType(Type referent, const ASTContext *C, \
                    RecursiveTypeProperties properties) \
    : ReferenceStorageType(TypeKind::Name##Storage, referent, C, properties){} \
public: \
  static Name##StorageType *get(Type referent, const ASTContext &C) { \
    return static_cast<Name##StorageType *>( \
        ReferenceStorageType::get(referent, ReferenceOwnership::Name, C)); \
  } \
  isLoadable \
  static bool classof(const TypeBase *T) { \
    return T->getKind() == TypeKind::Name##Storage; \
  } \
}; \
BEGIN_CAN_TYPE_WRAPPER(Name##StorageType, ReferenceStorageType) \
  static Can##Name##StorageType get(CanType referent) { \
    return cast<Name##StorageType>( \
        CanType(Name##StorageType::get(referent, referent->getASTContext()))); \
  } \
END_CAN_TYPE_WRAPPER(Name##StorageType, ReferenceStorageType)
#define UNCHECKED_REF_STORAGE(Name, ...) \
  REF_STORAGE_HELPER(Name, )
#define ALWAYS_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  REF_STORAGE_HELPER(Name, )
#define NEVER_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  REF_STORAGE_HELPER(Name, )
#define SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  REF_STORAGE_HELPER(Name, bool isLoadable(ResilienceExpansion resilience) const;)
#include "swift/AST/ReferenceStorage.def"
#undef REF_STORAGE_HELPER

/// A type variable used during type checking.
class alignas(1 << TypeVariableAlignInBits)
TypeVariableType : public TypeBase {
  // Note: We can't use llvm::TrailingObjects here because the trailing object
  // type is opaque.

  TypeVariableType(const ASTContext &C, unsigned ID)
      : TypeBase(TypeKind::TypeVariable, &C,
                 RecursiveTypeProperties::HasTypeVariable |
                     RecursiveTypeProperties::SolverAllocated) {
    // Note: the ID may overflow (current limit is 2^20 - 1).
    Bits.TypeVariableType.ID = ID;
    if (Bits.TypeVariableType.ID != ID) {
      llvm::report_fatal_error("Type variable id overflow");
    }
  }

  class Implementation;
  
public:
 
  /// Create a new type variable whose implementation is constructed
  /// with the given arguments.
  template<typename ...Args>
  static TypeVariableType *getNew(const ASTContext &C, unsigned ID,
                                  Args &&...args);
  
  /// Retrieve the implementation data corresponding to this type
  /// variable.
  ///
  /// The contents of the implementation data for this type are hidden in the
  /// details of the constraint solver used for type checking.
  Implementation &getImpl() {
    return *reinterpret_cast<Implementation *>(this + 1);
  }

  /// Retrieve the implementation data corresponding to this type
  /// variable.
  ///
  /// The contents of the implementation data for this type are hidden in the
  /// details of the constraint solver used for type checking.
  const Implementation &getImpl() const {
    return *reinterpret_cast<const Implementation *>(this + 1);
  }

  /// Access the implementation object for this type variable.
  Implementation *operator->() {
    return reinterpret_cast<Implementation *>(this + 1);
  }

  /// Type variable IDs are not globally unique and are
  /// used in equivalence class merging (so representative
  /// is always a type variable with smaller id), as well
  /// as a visual aid when dumping AST.
  unsigned getID() const { return Bits.TypeVariableType.ID; }

  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::TypeVariable;
  }
};
DEFINE_EMPTY_CAN_TYPE_WRAPPER(TypeVariableType, Type)

/// PlaceholderType - This represents a placeholder type for a type variable
/// or dependent member type that cannot be resolved to a concrete type
/// because the expression is ambiguous. This type is only used by the
/// constraint solver and transformed into UnresolvedType to be used in AST.
class PlaceholderType : public TypeBase {
  // NOTE: If you add a new Type-based originator, you'll need to update the
  // recursive property logic in PlaceholderType::get.
  using Originator =
      llvm::PointerUnion<TypeVariableType *, DependentMemberType *, VarDecl *,
                         ErrorExpr *, PlaceholderTypeRepr *>;

  Originator O;

  PlaceholderType(ASTContext &C, Originator originator,
                  RecursiveTypeProperties properties)
      : TypeBase(TypeKind::Placeholder, &C, properties), O(originator) {}

public:
  static Type get(ASTContext &ctx, Originator originator);

  Originator getOriginator() const { return O; }

  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::Placeholder;
  }
};
DEFINE_EMPTY_CAN_TYPE_WRAPPER(PlaceholderType, Type)

/// PackType - The type of a pack of arguments provided to a
/// \c PackExpansionType to guide the pack expansion process.
///
/// A pack type looks a lot like a tuple in the surface language, except there
/// is no way for the user to spell a pack. Pack types are created by the solver
/// when it encounters an apply of a variadic generic function, as in
///
/// \code
/// func print<T...>(_ xs: T...) {}
/// // Creates a pack type <String, Int, String>
/// print("Macs say Hello in", 42, " different languages")
/// \endcode
///
/// Pack types substituted into the variadic generic arguments of a
/// \c PackExpansionType "trip" the pack expansion and cause it to produce a
/// new pack type with the pack expansion pattern applied.
///
/// \code
/// typealias Foo<T...> = (T?...)
/// Foo<Int, String, Int> // Forces expansion to (Int?, String?, Int?)
/// \endcode
class PackType final : public TypeBase, public llvm::FoldingSetNode,
    private llvm::TrailingObjects<PackType, Type> {
  friend class ASTContext;
  friend TrailingObjects;

public:
  /// Creates a new, empty pack.
  static PackType *getEmpty(const ASTContext &C);
  /// Creates a pack from the types in \p elements.
  static PackType *get(const ASTContext &C, ArrayRef<Type> elements);

  /// Given a type T, which must be a pack parameter, a member type
  /// of a pack parameter, or a pack archetype, construct the type
  /// Pack{repeat each T}.
  static PackType *getSingletonPackExpansion(Type packParameter);

  static SmallVector<Type, 2> getExpandedGenericArgs(
      TypeArrayView<GenericTypeParamType> params,
      ArrayRef<Type> args);

public:
  /// Retrieves the number of elements in this pack.
  unsigned getNumElements() const { return Bits.PackType.Count; }

  /// Retrieves the type of the elements in the pack.
  ArrayRef<Type> getElementTypes() const {
    return {getTrailingObjects<Type>(), getNumElements()};
  }

  /// Returns the type of the element at the given \p index.
  Type getElementType(unsigned index) const {
    return getTrailingObjects<Type>()[index];
  }

  bool containsPackExpansionType() const;

  PackExpansionType *unwrapSingletonPackExpansion() const;

  CanTypeWrapper<PackType> getReducedShape();

public:
  void Profile(llvm::FoldingSetNodeID &ID) const {
    Profile(ID, getElementTypes());
  }
  static void Profile(llvm::FoldingSetNodeID &ID, ArrayRef<Type> Elements);

  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::Pack;
  }

private:
  PackType(ArrayRef<Type> elements, const ASTContext *CanCtx,
           RecursiveTypeProperties properties)
     : TypeBase(TypeKind::Pack, CanCtx, properties) {
     Bits.PackType.Count = elements.size();
     std::uninitialized_copy(elements.begin(), elements.end(),
                             getTrailingObjects<Type>());
  }
};
BEGIN_CAN_TYPE_WRAPPER(PackType, Type)
  static CanPackType get(const ASTContext &ctx, ArrayRef<CanType> elements);
  static CanPackType get(const ASTContext &ctx, CanTupleEltTypeArrayRef elts);
  static CanPackType get(const ASTContext &ctx,
                         AnyFunctionType::CanParamArrayRef params);

  static CanTypeWrapper<PackType>
  getSingletonPackExpansion(CanType packParameter);

  CanType getElementType(unsigned elementNo) const {
    return CanType(getPointer()->getElementType(elementNo));
  }

  CanTypeArrayRef getElementTypes() const {
    return CanTypeArrayRef(getPointer()->getElementTypes());
  }

  CanTypeWrapper<PackExpansionType> unwrapSingletonPackExpansion() const;
END_CAN_TYPE_WRAPPER(PackType, Type)

inline CanPackType CanTupleType::getInducedPackType() const {
  return getInducedPackTypeImpl(*this);
}

inline CanPackType
CanTupleType::getInducedPackType(unsigned start, unsigned end) const {
  return getInducedPackTypeImpl(*this, start, end);
}

inline CanPackType
CanTupleType::getInducedApproximateFormalPackType() const {
  return getInducedApproximateFormalPackTypeImpl(*this);
}

/// PackExpansionType - The interface type of the explicit expansion of a
/// corresponding set of variadic generic parameters.
///
/// Pack expansions are spelled as single-element tuples with a single variadic
/// component in most contexts except functions where they are allowed to appear
/// without parentheses to match normal variadic declaration syntax.
///
/// \code
/// func expand<T...>(_ xs: T...) -> (T...)
///                         ~~~~     ~~~~~~
/// \endcode
///
/// A pack expansion type comes equipped with a pattern type spelled before
/// the ellipses - \c T in the examples above. This pattern type is the subject
/// of the expansion of the pack that is tripped when its variadic generic
/// parameter is substituted for a \c PackType.
class PackExpansionType : public TypeBase, public llvm::FoldingSetNode {
  friend class ASTContext;

  Type patternType;
  Type countType;

public:
  /// Create a pack expansion type from the given pattern type.
  ///
  /// It is not required that \p pattern actually contain a reference to
  /// a variadic generic parameter, but any variadic generic parameters
  /// appearing in the pattern type must have the same count as \p countType.
  ///
  /// As for \p countType itself, it must be a type parameter pack
  /// type, or a pack archetype type.
  static PackExpansionType *get(Type pattern, Type countType);

public:
  /// Retrieves the pattern type of this pack expansion.
  Type getPatternType() const { return patternType; }

  /// Retrieves the count type of this pack expansion.
  Type getCountType() const { return countType; }

  CanType getReducedShape();

public:
  void Profile(llvm::FoldingSetNodeID &ID) {
    Profile(ID, getPatternType(), getCountType());
  }

  static void Profile(llvm::FoldingSetNodeID &ID,
                      Type patternType, Type countType);

  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::PackExpansion;
  }

private:
  PackExpansionType(Type patternType, Type countType,
                    RecursiveTypeProperties properties,
                    const ASTContext *ctx);
};
BEGIN_CAN_TYPE_WRAPPER(PackExpansionType, Type)
  static CanPackExpansionType get(CanType pattern, CanType countType);

  CanType getPatternType() const {
    return CanType(getPointer()->getPatternType());
  }

  CanType getCountType() const {
    return CanType(getPointer()->getCountType());
  }
END_CAN_TYPE_WRAPPER(PackExpansionType, Type)


inline CanTypeWrapper<PackExpansionType>
CanPackType::unwrapSingletonPackExpansion() const {
  return CanPackExpansionType(
      getPointer()->unwrapSingletonPackExpansion());
}

/// getASTContext - Return the ASTContext that this type belongs to.
inline ASTContext &TypeBase::getASTContext() const {
  // If this type is canonical, it has the ASTContext in it.
  if (isCanonical())
    return *const_cast<ASTContext*>(Context);

  // getCanonicalType() on GenericFunctionType is very expensive;
  // instead we can more easily fish the ASTContext out of any
  // structural sub-component.
  if (auto *genericFnType = dyn_cast<GenericFunctionType>(this))
    return genericFnType->getGenericParams()[0]->getASTContext();

  // If not, canonicalize it to get the Context.
  return *const_cast<ASTContext*>(getCanonicalType()->Context);
}

inline bool TypeBase::isTypeVariableOrMember() {
  Type t(this);

  while (auto *memberTy = t->getAs<DependentMemberType>())
    t = memberTy->getBase();

  return t->is<TypeVariableType>();
}

inline bool TypeBase::isTypeParameter() {
  Type t(this);

  while (auto *memberTy = t->getAs<DependentMemberType>())
    t = memberTy->getBase();

  return t->is<GenericTypeParamType>();
}

inline bool TypeBase::isParameterPack() {
  Type t(this);

  while (auto *memberTy = t->getAs<DependentMemberType>())
    t = memberTy->getBase();

  return t->is<GenericTypeParamType>() &&
         t->castTo<GenericTypeParamType>()->isParameterPack();
}

// TODO: This will become redundant once InOutType is removed.
inline bool TypeBase::isMaterializable() {
  return !(hasLValueType() || is<InOutType>());
}

inline GenericTypeParamType *TypeBase::getRootGenericParam() {
  Type t(this);

  while (auto *memberTy = t->getAs<DependentMemberType>())
    t = memberTy->getBase();

  return t->castTo<GenericTypeParamType>();
}

inline bool TypeBase::isConstraintType() const {
  return getCanonicalType().isConstraintType();
}

inline bool CanType::isConstraintTypeImpl(CanType type) {
  return (isa<ProtocolType>(type) ||
          isa<ProtocolCompositionType>(type) ||
          isa<ParameterizedProtocolType>(type));
}

inline bool TypeBase::isExistentialType() {
  return getCanonicalType().isExistentialType();
}

inline bool TypeBase::isAnyExistentialType() {
  return getCanonicalType().isAnyExistentialType();
}

inline bool CanType::isExistentialTypeImpl(CanType type) {
  return isa<ProtocolType>(type) ||
         isa<ProtocolCompositionType>(type) ||
         isa<ExistentialType>(type) ||
         isa<ParameterizedProtocolType>(type);
}

inline bool CanType::isAnyExistentialTypeImpl(CanType type) {
  return isExistentialTypeImpl(type) || isa<ExistentialMetatypeType>(type);
}

inline bool TypeBase::isClassExistentialType() {
  CanType T = getCanonicalType();
  if (auto pt = dyn_cast<ProtocolType>(T))
    return pt->requiresClass();
  if (auto pct = dyn_cast<ProtocolCompositionType>(T))
    return pct->requiresClass();
  if (auto ppt = dyn_cast<ParameterizedProtocolType>(T))
    return ppt->requiresClass();
  if (auto existential = dyn_cast<ExistentialType>(T))
    return existential->requiresClass();
  return false;
}

inline bool TypeBase::isOpenedExistential() const {
  if (!hasOpenedExistential())
    return false;

  CanType T = getCanonicalType();
  return isa<OpenedArchetypeType>(T);
}

inline bool TypeBase::canDynamicallyBeOptionalType(bool includeExistential) {
  CanType T = getCanonicalType();
  auto isArchetypeOrExistential = isa<ArchetypeType>(T) ||
    (includeExistential && T.isExistentialType());

  return isArchetypeOrExistential && !T.isAnyClassReferenceType();
}

inline ClassDecl *TypeBase::getClassOrBoundGenericClass() const {
  return getCanonicalType().getClassOrBoundGenericClass();
}

inline ClassDecl *CanType::getClassOrBoundGenericClass() const {
  if (auto classTy = dyn_cast<ClassType>(*this))
    return classTy->getDecl();

  if (auto boundTy = dyn_cast<BoundGenericClassType>(*this))
    return boundTy->getDecl();

  return nullptr;
}

inline StructDecl *TypeBase::getStructOrBoundGenericStruct() {
  return getCanonicalType().getStructOrBoundGenericStruct();
}

inline StructDecl *CanType::getStructOrBoundGenericStruct() const {
  if (auto structTy = dyn_cast<StructType>(*this))
    return structTy->getDecl();

  if (auto boundTy = dyn_cast<BoundGenericStructType>(*this))
    return boundTy->getDecl();
  
  return nullptr;
}

inline EnumDecl *TypeBase::getEnumOrBoundGenericEnum() {
  return getCanonicalType().getEnumOrBoundGenericEnum();
}

inline EnumDecl *CanType::getEnumOrBoundGenericEnum() const {
  if (auto enumTy = dyn_cast<EnumType>(*this))
    return enumTy->getDecl();

  if (auto boundTy = dyn_cast<BoundGenericEnumType>(*this))
    return boundTy->getDecl();
  
  return nullptr;
}

inline NominalTypeDecl *TypeBase::getNominalOrBoundGenericNominal() {
  return getCanonicalType().getNominalOrBoundGenericNominal();
}

inline NominalTypeDecl *CanType::getNominalOrBoundGenericNominal() const {
  if (auto Ty = dyn_cast<NominalOrBoundGenericNominalType>(*this))
    return Ty->getDecl();
  if (auto Ty = dyn_cast<ExistentialType>(*this))
    return Ty->getConstraintType()->getNominalOrBoundGenericNominal();
  if (auto Ty = dyn_cast<ParameterizedProtocolType>(*this))
    return Ty->getBaseType()->getNominalOrBoundGenericNominal();
  return nullptr;
}

inline NominalTypeDecl *TypeBase::getAnyNominal() {
  return getCanonicalType().getAnyNominal();
}

inline Type TypeBase::getNominalParent() {
  if (auto existential = getAs<ExistentialType>())
    return existential->getConstraintType()->getNominalParent();
  if (auto ppt = getAs<ParameterizedProtocolType>())
    return ppt->getBaseType()->getNominalParent();
  return castTo<AnyGenericType>()->getParent();
}

inline GenericTypeDecl *TypeBase::getAnyGeneric() {
  return getCanonicalType().getAnyGeneric();
}

inline bool TypeBase::isBuiltinIntegerType(unsigned n) {
  if (auto intTy = dyn_cast<BuiltinIntegerType>(getCanonicalType()))
    return intTy->getWidth().isFixedWidth()
      && intTy->getWidth().getFixedWidth() == n;
  return false;
}

/// getInOutObjectType - For an inout type, retrieves the underlying object
/// type.  Otherwise, returns the type itself.
inline Type TypeBase::getInOutObjectType() {
  if (auto iot = getAs<InOutType>())
    return iot->getObjectType();
  return this;
}

/// getWithoutSpecifierType - For a non-materializable type
/// e.g. @lvalue or inout, retrieves the underlying object type.
/// Otherwise, returns the type itself.
inline Type TypeBase::getWithoutSpecifierType() {
  if (auto iot = getAs<InOutType>())
    return iot->getObjectType();
  if (auto lv = getAs<LValueType>())
    return lv->getObjectType();
  return this;
}

/// For a ReferenceStorageType like @unowned, this returns the referent.
/// Otherwise, it returns the type itself.
inline Type TypeBase::getReferenceStorageReferent() {
  if (auto rst = getAs<ReferenceStorageType>())
    return rst->getReferentType();
  return this;
}

inline CanType CanType::getReferenceStorageReferentImpl(CanType type) {
  if (auto refType = dyn_cast<ReferenceStorageType>(type))
    return refType.getReferentType();
  return type;
}

inline CanType CanType::getWithoutSpecifierTypeImpl(CanType type) {
  if (auto refType = dyn_cast<InOutType>(type))
    return refType.getObjectType();
  if (auto refType = dyn_cast<LValueType>(type))
    return refType.getObjectType();
  return type;
}

inline CanType CanType::getNominalParent() const {
  return cast<NominalOrBoundGenericNominalType>(*this).getParent();
}

inline bool CanType::isActuallyCanonicalOrNull() const {
  return getPointer() == nullptr ||
         getPointer() == llvm::DenseMapInfo<TypeBase *>::getEmptyKey() ||
         getPointer() == llvm::DenseMapInfo<TypeBase *>::getTombstoneKey() ||
         getPointer()->isCanonical();
}

inline TupleTypeElt TupleTypeElt::getWithName(Identifier name) const {
  return TupleTypeElt(getType(), name);
}

inline TupleTypeElt TupleTypeElt::getWithType(Type T) const {
  return TupleTypeElt(T, getName());
}

/// Create one from what's present in the parameter decl and type
inline ParameterTypeFlags ParameterTypeFlags::fromParameterType(
    Type paramTy, bool isVariadic, bool isAutoClosure, bool isNonEphemeral,
    ParamSpecifier ownership, bool isolated, bool isNoDerivative,
    bool compileTimeConst) {
  // FIXME(Remove InOut): The last caller that needs this is argument
  // decomposition.  Start by enabling the assertion there and fixing up those
  // callers, then remove this, then remove
  // ParameterTypeFlags::fromParameterType entirely.
  if (paramTy->is<InOutType>()) {
    assert(ownership == ParamSpecifier::Default ||
           ownership == ParamSpecifier::InOut);
    ownership = ParamSpecifier::InOut;
  }
  return {isVariadic, isAutoClosure, isNonEphemeral, ownership, isolated,
          isNoDerivative, compileTimeConst};
}

inline const Type *BoundGenericType::getTrailingObjectsPointer() const {
  if (auto ty = dyn_cast<BoundGenericStructType>(this))
    return ty->getTrailingObjects<Type>();
  if (auto ty = dyn_cast<BoundGenericEnumType>(this))
    return ty->getTrailingObjects<Type>();
  if (auto ty = dyn_cast<BoundGenericClassType>(this))
    return ty->getTrailingObjects<Type>();
  llvm_unreachable("Unhandled BoundGenericType!");
}

inline ArrayRef<AnyFunctionType::Param> AnyFunctionType::getParams() const {
  switch (getKind()) {
  case TypeKind::Function:
    return cast<FunctionType>(this)->getParams();
  case TypeKind::GenericFunction:
    return cast<GenericFunctionType>(this)->getParams();
  default:
    llvm_unreachable("Undefined function type");
  }
}

/// If this is a method in a type or extension thereof, return the
/// 'self' parameter.
///
/// \param isInitializingCtor Specifies whether we're computing the 'self'
/// type of an initializing constructor, which accepts an instance 'self'
/// rather than a metatype 'self'.
///
/// \param wantDynamicSelf Specifies whether the 'self' type should be
/// wrapped in a DynamicSelfType, which is the case for the 'self' parameter
/// type inside a class method returning 'Self'.
AnyFunctionType::Param computeSelfParam(AbstractFunctionDecl *AFD,
                                        bool isInitializingCtor=false,
                                        bool wantDynamicSelf=false);

#define TYPE(id, parent)
#define SUGARED_TYPE(id, parent) \
template <> \
constexpr bool TypeBase::isSugaredType<id##Type>() { \
  return true; \
}
#include "swift/AST/TypeNodes.def"

inline GenericParamKey::GenericParamKey(const GenericTypeParamType *p)
    : ParameterPack(p->isParameterPack()), Depth(p->getDepth()),
      Index(p->getIndex()) {}

inline TypeBase *TypeBase::getDesugaredType() {
  if (!isa<SugarType>(this))
    return this;
  return cast<SugarType>(this)->getSinglyDesugaredType()->getDesugaredType();
}

} // end namespace swift

namespace llvm {

// DenseMapInfo for BuiltinIntegerWidth.
template<>
struct DenseMapInfo<swift::BuiltinIntegerWidth> {
  using BuiltinIntegerWidth = swift::BuiltinIntegerWidth;
  
  static inline BuiltinIntegerWidth getEmptyKey() {
    return BuiltinIntegerWidth(BuiltinIntegerWidth::DenseMapEmpty);
  }
  
  static inline BuiltinIntegerWidth getTombstoneKey() {
    return BuiltinIntegerWidth(BuiltinIntegerWidth::DenseMapTombstone);
  }
  
  static unsigned getHashValue(BuiltinIntegerWidth w) {
    return DenseMapInfo<unsigned>::getHashValue(w.RawValue);
  }
  
  static bool isEqual(BuiltinIntegerWidth a, BuiltinIntegerWidth b) {
    return a == b;
  }
};

}

#endif
