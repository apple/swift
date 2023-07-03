//===--- TypeAccessScopeChecker.h - Computes access for Types ---*- C++ -*-===//
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

#ifndef SWIFT_SEMA_TYPEACCESSSCOPECHECKER_H
#define SWIFT_SEMA_TYPEACCESSSCOPECHECKER_H

#include "swift/AST/Decl.h"
#include "swift/AST/DeclContext.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/Type.h"
#include "swift/AST/TypeDeclFinder.h"
#include "swift/AST/TypeRepr.h"

namespace swift {
class SourceFile;

/// Computes the access scope where a Type or TypeRepr is available, which is
/// the intersection of all the scopes where the declarations referenced in the
/// Type or TypeRepr are available.
class TypeAccessScopeChecker {
  const SourceFile *File;
  bool TreatUsableFromInlineAsPublic;

  llvm::Optional<AccessScope> Scope = AccessScope::getPublic();
  ImportAccessLevel ImportRestriction = llvm::None;

  TypeAccessScopeChecker(const DeclContext *useDC,
                         bool treatUsableFromInlineAsPublic)
      : File(useDC->getParentSourceFile()),
  TreatUsableFromInlineAsPublic(treatUsableFromInlineAsPublic) {}

  bool visitDecl(const ValueDecl *VD) {
    if (isa<GenericTypeParamDecl>(VD))
      return true;

    auto AS = VD->getFormalAccessScope(File, TreatUsableFromInlineAsPublic);
    Scope = Scope->intersectWith(AS);

    auto targetModule = VD->getDeclContext()->getParentModule();
    if (targetModule != File->getParentModule()) {
      auto localImportRestriction = File->getImportAccessLevel(targetModule);

      if (localImportRestriction.has_value() &&
          (!ImportRestriction.has_value() ||
           localImportRestriction.value().accessLevel <
             ImportRestriction.value().accessLevel)) {
        ImportRestriction = localImportRestriction;
      }
    }

    return Scope.has_value();
  }

public:

  struct Result {
    llvm::Optional<AccessScope> Scope;
    ImportAccessLevel Import;
  };

  static Result
  getAccessScope(TypeRepr *TR, const DeclContext *useDC,
                 bool treatUsableFromInlineAsPublic = false) {
    TypeAccessScopeChecker checker(useDC, treatUsableFromInlineAsPublic);
    TR->walk(TypeReprIdentFinder([&](const IdentTypeRepr *typeRepr) {
      return checker.visitDecl(typeRepr->getBoundDecl());
    }));
    return {checker.Scope,
            checker.ImportRestriction};
  }

  static Result
  getAccessScope(Type T, const DeclContext *useDC,
                 bool treatUsableFromInlineAsPublic = false) {
    TypeAccessScopeChecker checker(useDC, treatUsableFromInlineAsPublic);
    T.walk(SimpleTypeDeclFinder([&](const ValueDecl *VD) {
      if (checker.visitDecl(VD))
        return TypeWalker::Action::Continue;
      return TypeWalker::Action::Stop;
    }));

    return {checker.Scope,
            checker.ImportRestriction};
  }
};

} // end namespace swift

#endif
