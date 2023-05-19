//===--- DeclAndTypePrinter.h - Emit ObjC decls from Swift AST --*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_PRINTASCLANG_DECLANDTYPEPRINTER_H
#define SWIFT_PRINTASCLANG_DECLANDTYPEPRINTER_H

#include "OutputLanguageMode.h"

#include "swift/AST/Type.h"
// for OptionalTypeKind
#include "swift/ClangImporter/ClangImporter.h"
#include "llvm/ADT/StringSet.h"

namespace clang {
  class NamedDecl;
} // end namespace clang

namespace swift {

class PrimitiveTypeMapping;
class ValueDecl;
class SwiftToClangInteropContext;

/// Tracks which C++ declarations have been emitted in a lexical
/// C++ scope.
struct CxxDeclEmissionScope {
  /// Additional Swift declarations that are unrepresentable in C++.
  std::vector<const ValueDecl *> additionalUnrepresentableDeclarations;
  /// Records the C++ declaration names already emitted in this lexical scope.
  llvm::StringSet<> emittedDeclarationNames;
  /// Records the names of the function overloads already emitted in this
  /// lexical scope.
  llvm::StringMap<llvm::SmallVector<const AbstractFunctionDecl *, 2>>
      emittedFunctionOverloads;
};

/// Responsible for printing a Swift Decl or Type in Objective-C, to be
/// included in a Swift module's ObjC compatibility header.
class DeclAndTypePrinter {
public:
  using DelayedMemberSet = llvm::SmallSetVector<const ValueDecl *, 32>;

private:
  class Implementation;
  friend class Implementation;

  ModuleDecl &M;
  raw_ostream &os;
  raw_ostream &prologueOS;
  raw_ostream &outOfLineDefinitionsOS;
  const DelayedMemberSet &delayedMembers;
  CxxDeclEmissionScope *cxxDeclEmissionScope;
  PrimitiveTypeMapping &typeMapping;
  SwiftToClangInteropContext &interopContext;
  AccessLevel minRequiredAccess;
  bool requiresExposedAttribute;
  llvm::StringSet<> &exposedModules;
  OutputLanguageMode outputLang;

  /// The name 'CFTypeRef'.
  ///
  /// Cached for convenience.
  Identifier ID_CFTypeRef;

  Implementation getImpl();

public:
  DeclAndTypePrinter(ModuleDecl &mod, raw_ostream &out, raw_ostream &prologueOS,
                     raw_ostream &outOfLineDefinitionsOS,
                     DelayedMemberSet &delayed,
                     CxxDeclEmissionScope &topLevelEmissionScope,
                     PrimitiveTypeMapping &typeMapping,
                     SwiftToClangInteropContext &interopContext,
                     AccessLevel access, bool requiresExposedAttribute,
                     llvm::StringSet<> &exposedModules,
                     OutputLanguageMode outputLang)
      : M(mod), os(out), prologueOS(prologueOS),
        outOfLineDefinitionsOS(outOfLineDefinitionsOS), delayedMembers(delayed),
        cxxDeclEmissionScope(&topLevelEmissionScope), typeMapping(typeMapping),
        interopContext(interopContext), minRequiredAccess(access),
        requiresExposedAttribute(requiresExposedAttribute),
        exposedModules(exposedModules), outputLang(outputLang) {}

  PrimitiveTypeMapping &getTypeMapping() { return typeMapping; }

  SwiftToClangInteropContext &getInteropContext() { return interopContext; }

  CxxDeclEmissionScope &getCxxDeclEmissionScope() {
    return *cxxDeclEmissionScope;
  }

  void setCxxDeclEmissionScope(CxxDeclEmissionScope &scope) {
    cxxDeclEmissionScope = &scope;
  }

  /// Returns true if \p VD should be included in a compatibility header for
  /// the options the printer was constructed with.
  bool shouldInclude(const ValueDecl *VD);

  /// Returns true if \p vd is visible given the current access level and thus
  /// can be included in the generated header.
  bool isVisible(const ValueDecl *vd) const;

  void print(const Decl *D);
  void print(Type ty);

  void printAvailability(raw_ostream &os, const Decl *D);

  /// Is \p ED empty of members and protocol conformances to include?
  bool isEmptyExtensionDecl(const ExtensionDecl *ED);

  /// Returns the type that will be printed by PrintAsObjC for a parameter or
  /// result type resolved to this declaration.
  ///
  /// \warning This handles \c _ObjectiveCBridgeable types, but it doesn't
  /// currently know about other aspects of PrintAsObjC behavior, like known
  /// types.
  const TypeDecl *getObjCTypeDecl(const TypeDecl* TD);

  /// Prints a category declaring the given members.
  ///
  /// All members must have the same parent type. The list must not be empty.
  void
  printAdHocCategory(iterator_range<const ValueDecl * const *> members);

  /// Returns the name of an <os/object.h> type minus the leading "OS_",
  /// or an empty string if \p decl is not an <os/object.h> type.
  static StringRef maybeGetOSObjectBaseName(const clang::NamedDecl *decl);

  static std::pair<Type, OptionalTypeKind>
  getObjectTypeAndOptionality(const ValueDecl *D, Type ty);
};

} // end namespace swift

#endif
