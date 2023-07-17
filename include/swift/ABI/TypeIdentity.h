//===--- TypeIdentity.h - Identity info about imported types -----*- C++ -*-==//
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
// This header declares structures useful for working with the identity of
// a type, especially for imported types.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_ABI_TYPEIDENTITY_H
#define SWIFT_ABI_TYPEIDENTITY_H

#include "swift/Basic/LLVM.h"
#include "swift/Runtime/Config.h"
#include <llvm/ADT/Optional.h>
#include <llvm/ADT/StringRef.h>

namespace swift {
template <typename Runtime>
class swift_ptrauth_struct_context_descriptor(TypeContextDescriptor)
    TargetTypeContextDescriptor;
struct InProcess;
using TypeContextDescriptor = TargetTypeContextDescriptor<InProcess>;

/// The different components that can appear in a TypeImportInfo sequence.
///
/// The declaration order here is the canonical order for these components
/// in the sequence; the sequence is ill-formed if they are out of order.
///
/// Note that the ABI name (or the ordinary formal name, which
/// immediately precedes it), symbol namespace, and related entity
/// name together form the identity of the TypeImportInfo.
enum class TypeImportComponent : char {
  ABIName = 'N',
  SymbolNamespace = 'S',
  RelatedEntityName = 'R',
};

namespace TypeImportSymbolNamespace {

/// A value for `SymbolNamespace` which indicates that this type came
/// from a C `typedef` that was imported as a distinct type instead
/// of a `typealias`.  This can happen for reasons like:
///
/// - the `typedef` was declared with the `swift_wrapper` attribute
/// - the `typedef` is a CF type
constexpr static const char CTypedef[] = "t";

}

/// A class containing information from the type-import info.
template <class StringType>
class TypeImportInfo {
public:
  /// The ABI name of the declaration, if different from the user-facing
  /// name.
  StringType ABIName;

  /// Set if the type doesn't come from the default symbol namespace for
  /// its kind and source language.  (Source language can be determined
  /// from the parent context.)
  ///
  /// Some languages (most importantly, C/C++/Objective-C) have different
  /// symbol namespaces in which types can be declared; for example,
  /// `struct A` and `typedef ... A` can be declared in the same scope and
  /// resolve to unrelated types.  When these declarations are imported,
  /// there are several possible ways to distinguish them in Swift, e.g.
  /// by implicitly renaming them; however, the external name used for
  /// mangling and metadata must be stable and so is based on the original
  /// declared name.  Therefore, in these languages, we privilege one
  /// symbol namespace as the default (although which may depend on the
  /// type kind), and declarations from the other(s) must be marked in
  /// order to differentiate them.
  ///
  /// C, C++, and Objective-C
  /// -----------------------
  ///
  /// C, C++, and Objective-C have several different identifier namespaces
  /// that can declare types: the ordinary namespace (`typedef`s and ObjC
  /// `@interface`s), the tag namespace (`struct`s, `enum`s, `union`s, and
  /// C++ `class`es), and the ObjC protocol namespace (ObjC `@protocol`s).
  /// The languages all forbid multiple types from being declared in a given
  /// scope and namespace --- at least, they do within a translation unit,
  /// and for the most part we have to assume in Swift that that applies
  /// across translation units as well.
  //
  /// Swift's default symbol-namespace rules for C/C++/ObjC are as follows:
  /// - Protocols are assumed to come from the ObjC protocol namespace.
  /// - Classes are assumed to come from the ordinary namespace (as an
  ///   Objective-C class would).
  /// - Structs and enums are assumed to come from the tag namespace
  ///   (as a C `struct`, `union`, or `enum` would).
  //;
  /// Note that there are some special mangling rules for types imported
  /// from C tag types in addition to the symbol-namespace rules.
  StringType SymbolNamespace;

  /// Set if the type is an importer-synthesized related entity.
  /// A related entity is an entity synthesized in response to an imported
  /// type which is not the type itself; for example, when the importer
  /// sees an ObjC error domain, it creates an error-wrapper type (a
  /// related entity) and a `Code` enum (not a related entity because it's
  /// exactly the original type).
  ///
  /// The name and import namespace (together with the parent context)
  /// identify the original declaration.
  StringType RelatedEntityName;

  /// Attempt to collect information from the given info chunk.
  ///
  /// \return true if collection was successful.
  template <bool Asserting>
  bool collect(llvm::StringRef value) {
#define check(CONDITION, COMMENT)            \
    do {                                     \
      if (!Asserting) {                      \
        if (!(CONDITION)) return false;      \
      } else {                               \
        assert((CONDITION) && COMMENT);      \
      }                                      \
    } while (false)

    check(!value.empty(), "string was empty on entrance");
    auto component = TypeImportComponent(value[0]);
    value = value.drop_front(1);

    switch (component) {
#define case_setIfNonEmpty(FIELD)                                              \
  case TypeImportComponent::FIELD:                                             \
    check(!value.empty(), "incoming value of " #FIELD " was empty");           \
    check(FIELD.empty(), #FIELD " was already set");                           \
    FIELD = StringType(value);                                                 \
    return true;

      case_setIfNonEmpty(ABIName)
      case_setIfNonEmpty(SymbolNamespace)
      case_setIfNonEmpty(RelatedEntityName)

#undef case_setIfNonEmpty
#undef check
    }

    // Even with Asserting=true (i.e. in the runtime), we do want to be
    // future-proof against new components.
    return false;
  }

  /// Append all the information in this structure to the given buffer,
  /// including all necessary internal and trailing '\0' characters.
  /// The buffer is assumed to already contain the '\0'-terminated
  /// user-facing name of the type.
  template <class BufferTy>
  void appendTo(BufferTy &buffer) const {
#define append(FIELD)                               \
    do {                                            \
      if (!FIELD.empty()) {                         \
        buffer += char(TypeImportComponent::FIELD); \
        buffer += FIELD;                            \
        buffer += '\0';                             \
      }                                             \
    } while (false)

    append(ABIName);
    append(SymbolNamespace);
    append(RelatedEntityName);
    buffer += '\0';

#undef append
  }
};

/// Parsed information about the identity of a type.
class ParsedTypeIdentity {
public:
  /// The user-facing name of the type.
  llvm::StringRef UserFacingName;

  /// The full identity of the type.
  /// Note that this may include interior '\0' characters.
  llvm::StringRef FullIdentity;

  /// Any extended information that type might have.
  llvm::Optional<TypeImportInfo<llvm::StringRef>> ImportInfo;

  /// The ABI name of the type.
  llvm::StringRef getABIName() const {
    if (ImportInfo && !ImportInfo->ABIName.empty())
      return ImportInfo->ABIName;
    return UserFacingName;
  }

  bool isCTypedef() const {
    return ImportInfo &&
           ImportInfo->SymbolNamespace == TypeImportSymbolNamespace::CTypedef;
  }

  bool isAnyRelatedEntity() const {
    return ImportInfo && !ImportInfo->RelatedEntityName.empty();
  }

  bool isRelatedEntity(llvm::StringRef entityName) const {
    return ImportInfo && ImportInfo->RelatedEntityName == entityName;
  }

  llvm::StringRef getRelatedEntityName() const {
    assert(isAnyRelatedEntity());
    return ImportInfo->RelatedEntityName;
  }

  static ParsedTypeIdentity parse(const TypeContextDescriptor *type);
};

} // end namespace swift

#endif // SWIFT_ABI_TYPEIDENTITY_H
