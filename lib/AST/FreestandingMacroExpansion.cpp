//===--- FreestandingMacroExpansion.cpp -----------------------------------===//
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

#include "swift/AST/FreestandingMacroExpansion.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Expr.h"

using namespace swift;

SourceRange MacroExpansionInfo::getSourceRange() const {
  SourceLoc endLoc;
  if (ArgList && !ArgList->isImplicit())
    endLoc = ArgList->getEndLoc();
  else if (RightAngleLoc.isValid())
    endLoc = RightAngleLoc;
  else
    endLoc = MacroNameLoc.getEndLoc();

  return SourceRange(SigilLoc, endLoc);
}

#define FORWARD_VARIANT(NAME)                                                  \
  switch (getFreestandingMacroKind()) {                                        \
  case FreestandingMacroKind::Expr:                                            \
    return cast<MacroExpansionExpr>(this)->NAME();                             \
  case FreestandingMacroKind::Decl:                                            \
    return cast<MacroExpansionDecl>(this)->NAME();                             \
  }

DeclContext *FreestandingMacroExpansion::getDeclContext() const {
  FORWARD_VARIANT(getDeclContext);
}
SourceRange FreestandingMacroExpansion::getSourceRange() const {
  FORWARD_VARIANT(getSourceRange);
}
unsigned FreestandingMacroExpansion::getDiscriminator() const {
  FORWARD_VARIANT(getDiscriminator);
}

ASTNode FreestandingMacroExpansion::getASTNode() {
  switch (getFreestandingMacroKind()) {
  case FreestandingMacroKind::Expr:
    return cast<MacroExpansionExpr>(this);
  case FreestandingMacroKind::Decl:
    return cast<MacroExpansionDecl>(this);
  }
}
