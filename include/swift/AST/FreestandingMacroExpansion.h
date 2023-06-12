//===--- FreestandingMacroExpansion.h ------------------------*- C++ -*-===//
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

#ifndef SWIFT_AST_FREESTANDING_MACRO_EXPANSION_H
#define SWIFT_AST_FREESTANDING_MACRO_EXPANSION_H

#include "swift/AST/ASTAllocated.h"
#include "swift/AST/ASTNode.h"
#include "swift/AST/ConcreteDeclRef.h"
#include "swift/AST/DeclNameLoc.h"
#include "swift/AST/Identifier.h"
#include "swift/Basic/SourceLoc.h"

namespace swift {
class MacroExpansionDecl;
class MacroExpansionExpr;
class Expr;
class Decl;
class ArgumentList;

/// Information about a macro expansion that is common between macro
/// expansion declarations and expressions.
///
/// Instances of these types will be shared among paired macro expansion
/// declaration/expression nodes.
struct MacroExpansionInfo : ASTAllocated<MacroExpansionInfo> {
  SourceLoc SigilLoc;
  DeclNameRef MacroName;
  DeclNameLoc MacroNameLoc;
  SourceLoc LeftAngleLoc, RightAngleLoc;
  llvm::ArrayRef<TypeRepr *> GenericArgs;
  ArgumentList *ArgList;

  /// The referenced macro.
  ConcreteDeclRef macroRef;

  MacroExpansionInfo(SourceLoc sigilLoc, DeclNameRef macroName,
                     DeclNameLoc macroNameLoc, SourceLoc leftAngleLoc,
                     SourceLoc rightAngleLoc, ArrayRef<TypeRepr *> genericArgs,
                     ArgumentList *argList)
      : SigilLoc(sigilLoc), MacroName(macroName), MacroNameLoc(macroNameLoc),
        LeftAngleLoc(leftAngleLoc), RightAngleLoc(rightAngleLoc),
        GenericArgs(genericArgs), ArgList(argList) {}

  SourceLoc getLoc() const { return SigilLoc; }
  SourceRange getGenericArgsRange() const {
    return {LeftAngleLoc, RightAngleLoc};
  }
  SourceRange getSourceRange() const;
};

enum class FreestandingMacroKind {
  Expr, // MacroExpansionExpr.
  Decl, // MacroExpansionDecl.
};

/// A base class of either 'MacroExpansionExpr' or 'MacroExpansionDecl'.
class FreestandingMacroExpansion {
  llvm::PointerIntPair<MacroExpansionInfo *, 1, FreestandingMacroKind>
      infoAndKind;

protected:
  FreestandingMacroExpansion(FreestandingMacroKind kind,
                             MacroExpansionInfo *info)
      : infoAndKind(info, kind) {}

public:
  MacroExpansionInfo *getExpansionInfo() const {
    return infoAndKind.getPointer();
  }
  FreestandingMacroKind getFreestandingMacroKind() const {
    return infoAndKind.getInt();
  }

  ASTNode getASTNode();

  SourceLoc getPoundLoc() const { return getExpansionInfo()->SigilLoc; }

  DeclNameLoc getMacroNameLoc() const {
    return getExpansionInfo()->MacroNameLoc;
  }
  DeclNameRef getMacroName() const { return getExpansionInfo()->MacroName; }

  ArrayRef<TypeRepr *> getGenericArgs() const {
    return getExpansionInfo()->GenericArgs;
  }
  SourceRange getGenericArgsRange() const {
    return getExpansionInfo()->getGenericArgsRange();
  }

  ArgumentList *getArgs() const { return getExpansionInfo()->ArgList; }
  void setArgs(ArgumentList *args) { getExpansionInfo()->ArgList = args; }

  ConcreteDeclRef getMacroRef() const { return getExpansionInfo()->macroRef; }
  void setMacroRef(ConcreteDeclRef ref) { getExpansionInfo()->macroRef = ref; }

  DeclContext *getDeclContext() const;
  SourceRange getSourceRange() const;
  unsigned getDiscriminator() const;
};

} // namespace swift

#endif // SWIFT_AST_FREESTANDING_MACRO_EXPANSION_H
