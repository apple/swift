//===--- SwiftNameTranslation.h - Swift Name Translation --------*- C++ -*-===//
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

#ifndef SWIFT_NAME_TRANSLATION_H
#define SWIFT_NAME_TRANSLATION_H

#include "swift/AST/AttrKind.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/Identifier.h"

namespace swift {
  class ValueDecl;
  class EnumDecl;
  class EnumElementDecl;

namespace objc_translation {
  enum CustomNamesOnly_t : bool {
    Normal = false,
    CustomNamesOnly = true,
  };

  StringRef getNameForObjC(const ValueDecl *VD,
                           CustomNamesOnly_t customNamesOnly = Normal);

  std::string getErrorDomainStringForObjC(const EnumDecl *ED);

  /// Print the ObjC name of an enum element decl to OS, also allowing the client
  /// to specify a preferred name other than the decl's original name.
  ///
  /// Returns true if the decl has a custom ObjC name (@objc); false otherwise.
  bool printSwiftEnumElemNameInObjC(const EnumElementDecl *EL,
                                    llvm::raw_ostream &OS,
                                    Identifier PreferredName = Identifier());

  /// Get the name for a value decl D if D is exported to ObjC, PreferredName is
  /// specified to perform what-if analysis, shadowing D's original name during
  /// computation.
  ///
  /// Returns a pair of Identifier and ObjCSelector, only one of which is valid.
  std::pair<Identifier, ObjCSelector>
  getObjCNameForSwiftDecl(const ValueDecl *VD, DeclName PreferredName = DeclName());

  /// Returns true if the given value decl D is visible to ObjC of its
  /// own accord (i.e. without considering its context)
  bool isVisibleToObjC(const ValueDecl *VD, AccessLevel minRequiredAccess,
                       bool checkParent = true);

} // end namespace objc_translation

namespace cxx_translation {

using objc_translation::CustomNamesOnly_t;

StringRef
getNameForCxx(const ValueDecl *VD,
              CustomNamesOnly_t customNamesOnly = objc_translation::Normal);

enum RepresentationKind { Representable, Unsupported };

enum RepresentationError {
  UnrepresentableObjC,
  UnrepresentableAsync,
  UnrepresentableIsolatedInActor,
  UnrepresentableRequiresClientEmission,
  UnrepresentableGeneric,
  UnrepresentableGenericRequirements,
  UnrepresentableThrows,
  UnrepresentableIndirectEnum,
  UnrepresentableEnumCaseType,
  UnrepresentableEnumCaseTuple,
  UnrepresentableProtocol,
  UnrepresentableMoveOnly,
};

/// Constructs a diagnostic that describes the given C++ representation error.
Diagnostic diagnoseRepresenationError(RepresentationError error, ValueDecl *vd);

struct DeclRepresentation {
  RepresentationKind kind;
  llvm::Optional<RepresentationError> error;

  /// Returns true if the given Swift node is unsupported in Clang in any
  /// language mode.
  bool isUnsupported() const { return kind == Unsupported; }
};

/// Returns the C++ representation info for the given declaration.
DeclRepresentation getDeclRepresentation(const ValueDecl *VD);

/// Returns true if the given value decl is exposable to C++.
inline bool isExposableToCxx(const ValueDecl *VD) {
  return !getDeclRepresentation(VD).isUnsupported();
}

/// Returns true if the given value decl D is visible to C++ of its
/// own accord (i.e. without considering its context)
bool isVisibleToCxx(const ValueDecl *VD, AccessLevel minRequiredAccess,
                    bool checkParent = true);

} // end namespace cxx_translation

} // end namespace swift

#endif
