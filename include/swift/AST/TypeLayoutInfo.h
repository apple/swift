//===--- TypeLayoutInfo.h ---------------------------------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file defines an interface for obtaining information about a type's
//  layout. A "layout" is some property of the type's representation in-memory.
//
//  For example, whether a type is considered plain-old-data (POD) is a
//  property of a representation of the type chosen by the compiler as
//  part of how values are represented in-memory.
//
//  In particular, this file provides such layout information to both Sema
//  and SIL, serving as the single-source of information about the layout of
//  a type.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_INCLUDE_SWIFT_AST_TYPELAYOUTINFO_H
#define SWIFT_INCLUDE_SWIFT_AST_TYPELAYOUTINFO_H

#include "swift/AST/Type.h"
#include "swift/AST/TypeExpansionContext.h"

namespace swift {
namespace Lowering {

/// Is a lowered SIL type trivial?  That is, are copies ultimately just
/// bit-copies, and it takes no work to destroy a value?
enum IsTrivial_t : bool {
  IsNotTrivial = false,
  IsTrivial = true
};

/// Is a lowered SIL type the Builtin.RawPointer or a struct/tuple/enum which
/// contains a Builtin.RawPointer?
/// HasRawPointer is true only for types that are known to contain
/// Builtin.RawPointer. It is not assumed true for generic or resilient types.
enum HasRawPointer_t : bool {
  DoesNotHaveRawPointer = false,
  HasRawPointer = true
};

/// Is a lowered SIL type fixed-ABI?  That is, can the current context
/// assign it a fixed size and alignment and perform value operations on it
/// (such as copies, destroys, constructions, and projections) without
/// metadata?
///
/// Note that a fully concrete type can be non-fixed-ABI without being
/// itself resilient if it contains a subobject which is not fixed-ABI.
///
/// Also note that we're only concerned with the external value ABI here:
/// resilient class types are still fixed-ABI, indirect enum cases do not
/// affect the fixed-ABI-ness of the enum, and so on.
enum IsFixedABI_t : bool {
  IsNotFixedABI = false,
  IsFixedABI = true
};

/// Is a lowered SIL type address-only?  That is, is the current context
/// required to keep the value in memory for some reason?
///
/// A type might be address-only because:
///
///   - it is not fixed-size (e.g. because it is a resilient type) and
///     therefore cannot be loaded into a statically-boundable set of
///     registers; or
///
///   - it is semantically bound to memory, either because its storage
///     address is used by the language runtime to implement its semantics
///     (as with a weak reference) or because its representation is somehow
///     address-dependent (as with something like a relative pointer).
///
/// An address-only type can be fixed-layout and/or trivial.
/// A non-fixed-layout type is always address-only.
enum IsAddressOnly_t : bool {
  IsNotAddressOnly = false,
  IsAddressOnly = true
};

/// Is this type somewhat like a reference-counted type?
enum IsReferenceCounted_t : bool {
  IsNotReferenceCounted = false,
  IsReferenceCounted = true
};

/// Is this type address only because it's resilient?
enum IsResilient_t : bool {
  IsNotResilient = false,
  IsResilient = true
};

/// Does this type contain an opaque result type that affects type lowering?
enum IsTypeExpansionSensitive_t : bool {
  IsNotTypeExpansionSensitive = false,
  IsTypeExpansionSensitive = true
};

/// Is the type infinitely defined in terms of itself? (Such types can never
/// be concretely instantiated, but may still arise from generic specialization.)
enum IsInfiniteType_t : bool {
  IsNotInfiniteType = false,
  IsInfiniteType = true,
};

/// Does this type contain at least one non-trivial, non-eager-move type?
enum IsLexical_t : bool {
  IsNotLexical = false,
  IsLexical = true,
};

/// TODO: describe this type
struct TypeLayoutInfo {
private:
  // These are chosen so that bitwise-or merges the flags properly.
  //
  // clang-format off
  enum : unsigned {
    NonTrivialFlag             = 1 << 0,
    NonFixedABIFlag            = 1 << 1,
    AddressOnlyFlag            = 1 << 2,
    ResilientFlag              = 1 << 3,
    TypeExpansionSensitiveFlag = 1 << 4,
    InfiniteFlag               = 1 << 5,
    HasRawPointerFlag          = 1 << 6,
    LexicalFlag                = 1 << 7,
  };
  // clang-format on

  uint8_t Flags;

  constexpr TypeLayoutInfo(uint8_t InitialFlags) : Flags(InitialFlags) {}
public:
  /// Construct a default TypeLayoutInfo, which corresponds to
  /// a trivial, loadable, fixed-layout type.
  constexpr TypeLayoutInfo() : Flags(0) {}

  constexpr TypeLayoutInfo(
      IsTrivial_t isTrivial, IsFixedABI_t isFixedABI,
      IsAddressOnly_t isAddressOnly, IsResilient_t isResilient,
      IsTypeExpansionSensitive_t isTypeExpansionSensitive =
      IsNotTypeExpansionSensitive,
      HasRawPointer_t hasRawPointer = DoesNotHaveRawPointer,
      IsLexical_t isLexical = IsNotLexical)
      : Flags((isTrivial ? 0U : NonTrivialFlag) |
      (isFixedABI ? 0U : NonFixedABIFlag) |
      (isAddressOnly ? AddressOnlyFlag : 0U) |
      (isResilient ? ResilientFlag : 0U) |
      (isTypeExpansionSensitive ? TypeExpansionSensitiveFlag : 0U) |
      (hasRawPointer ? HasRawPointerFlag : 0U) |
      (isLexical ? LexicalFlag : 0U)) {}

  constexpr bool operator==(TypeLayoutInfo p) const {
    return Flags == p.Flags;
  }

  static constexpr TypeLayoutInfo forTrivial() {
    return {IsTrivial, IsFixedABI, IsNotAddressOnly, IsNotResilient};
  }

  static constexpr TypeLayoutInfo forRawPointer() {
    return {IsTrivial, IsFixedABI, IsNotAddressOnly, IsNotResilient,
            IsNotTypeExpansionSensitive, HasRawPointer};
  }

  static constexpr TypeLayoutInfo forReference() {
    return {IsNotTrivial, IsFixedABI, IsNotAddressOnly, IsNotResilient,
            IsNotTypeExpansionSensitive, DoesNotHaveRawPointer, IsLexical};
  }

  static constexpr TypeLayoutInfo forOpaque() {
    return {IsNotTrivial, IsNotFixedABI, IsAddressOnly, IsNotResilient,
            IsNotTypeExpansionSensitive, DoesNotHaveRawPointer, IsLexical};
  }

  static constexpr TypeLayoutInfo forResilient() {
    return {IsTrivial, IsFixedABI, IsNotAddressOnly, IsResilient};
  }

  constexpr void addSubobject(TypeLayoutInfo other) {
    Flags |= other.Flags;
  }

  constexpr IsTrivial_t isTrivial() const {
    return IsTrivial_t((Flags & NonTrivialFlag) == 0);
  }
  constexpr HasRawPointer_t isOrContainsRawPointer() const {
    return HasRawPointer_t((Flags & HasRawPointerFlag) != 0);
  }
  constexpr IsFixedABI_t isFixedABI() const {
    return IsFixedABI_t((Flags & NonFixedABIFlag) == 0);
  }
  constexpr IsAddressOnly_t isAddressOnly() const {
    return IsAddressOnly_t((Flags & AddressOnlyFlag) != 0);
  }
  constexpr IsResilient_t isResilient() const {
    return IsResilient_t((Flags & ResilientFlag) != 0);
  }
  constexpr IsTypeExpansionSensitive_t isTypeExpansionSensitive() const {
    return IsTypeExpansionSensitive_t(
        (Flags & TypeExpansionSensitiveFlag) != 0);
  }
  constexpr IsInfiniteType_t isInfinite() const {
    return IsInfiniteType_t((Flags & InfiniteFlag) != 0);
  }
  constexpr IsLexical_t isLexical() const {
    return IsLexical_t((Flags & LexicalFlag) != 0);
  }

  constexpr void setNonTrivial() { Flags |= NonTrivialFlag; }
  constexpr void setNonFixedABI() { Flags |= NonFixedABIFlag; }
  constexpr void setAddressOnly() { Flags |= AddressOnlyFlag; }
  constexpr void setTypeExpansionSensitive(
      IsTypeExpansionSensitive_t isTypeExpansionSensitive) {
    Flags = (Flags & ~TypeExpansionSensitiveFlag) |
        (isTypeExpansionSensitive ? TypeExpansionSensitiveFlag : 0);
  }
  constexpr void setInfinite() { Flags |= InfiniteFlag; }
  constexpr void setLexical(IsLexical_t isLexical) {
    Flags = (Flags & ~LexicalFlag) | (isLexical ? LexicalFlag : 0);
  }

  constexpr TypeLayoutInfo withTypeExpansionSensitive(
      IsTypeExpansionSensitive_t isTypeExpansionSensitive) {
    TypeLayoutInfo prop(Flags);
    prop.setTypeExpansionSensitive(isTypeExpansionSensitive);
    return prop;
  }

  /// TODO: describe
  static TypeLayoutInfo get(ASTContext &ctx,
                            CanType type,
                            CanGenericSignature sig,
                            TypeExpansionContext expansion);

};

} // Lowering
} // swift

#endif //SWIFT_INCLUDE_SWIFT_AST_TYPELAYOUTINFO_H
