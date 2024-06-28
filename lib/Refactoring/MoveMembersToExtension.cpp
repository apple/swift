//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "RefactoringActions.h"
#include "swift/Basic/Assertions.h"

using namespace swift::refactoring;

bool RefactoringActionMoveMembersToExtension::isApplicable(
    const ResolvedRangeInfo &Info, DiagnosticEngine &Diag) {
  switch (Info.Kind) {
  case RangeKind::SingleDecl:
  case RangeKind::MultiTypeMemberDecl: {
    DeclContext *DC = Info.RangeContext;

    // The common decl context is not a nomial type, we cannot create an
    // extension for it
    if (!DC || !DC->getInnermostDeclarationDeclContext() ||
        !isa<NominalTypeDecl>(DC->getInnermostDeclarationDeclContext()))
      return false;

    // Members of types not declared at top file level cannot be extracted
    // to an extension at top file level
    if (DC->getParent()->getContextKind() != DeclContextKind::FileUnit)
      return false;

    // Check if contained nodes are all allowed decls.
    for (auto Node : Info.ContainedNodes) {
      Decl *D = Node.dyn_cast<Decl *>();
      if (!D)
        return false;

      if (isa<AccessorDecl>(D) || isa<DestructorDecl>(D) ||
          isa<EnumCaseDecl>(D) || isa<EnumElementDecl>(D))
        return false;
    }

    // We should not move instance variables with storage into the extension
    // because they are not allowed to be declared there
    for (auto DD : Info.DeclaredDecls) {
      if (auto ASD = dyn_cast<AbstractStorageDecl>(DD.VD)) {
        // Only disallow storages in the common decl context, allow them in
        // any subtypes
        if (ASD->hasStorage() && ASD->getDeclContext() == DC) {
          return false;
        }
      }
    }

    return true;
  }
  case RangeKind::SingleExpression:
  case RangeKind::PartOfExpression:
  case RangeKind::SingleStatement:
  case RangeKind::MultiStatement:
  case RangeKind::Invalid:
    return false;
  }
  llvm_unreachable("unhandled kind");
}

bool RefactoringActionMoveMembersToExtension::performChange() {
  DeclContext *DC = RangeInfo.RangeContext;

  auto CommonTypeDecl =
      dyn_cast<NominalTypeDecl>(DC->getInnermostDeclarationDeclContext());
  assert(CommonTypeDecl && "Not applicable if common parent is no nomial type");

  SmallString<64> Buffer;
  llvm::raw_svector_ostream OS(Buffer);
  OS << "\n\n";
  OS << "extension " << CommonTypeDecl->getName() << " {\n";
  OS << RangeInfo.ContentRange.str().trim();
  OS << "\n}";

  // Insert extension after the type declaration
  EditConsumer.insertAfter(SM, CommonTypeDecl->getEndLoc(), Buffer);
  EditConsumer.remove(SM, RangeInfo.ContentRange);

  return false;
}
