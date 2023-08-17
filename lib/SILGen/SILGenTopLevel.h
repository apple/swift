//===--- SILGenTopLevel.h - Top-level Code Emission -------------*- C++ -*-===//
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

#ifndef SWIFT_SILGEN_SILGENTOPLEVEL_H
#define SWIFT_SILGEN_SILGENTOPLEVEL_H

#include "SILGen.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/TypeMemberVisitor.h"

namespace swift {

namespace Lowering {

/// Generates a `SILFunction` for `TopLevelCodeDecl`s within a
/// source file ran in script mode.
class SILGenTopLevel : public ASTVisitor<SILGenTopLevel> {
public:
  /// Generate SIL for toplevel code into `SGF`
  SILGenTopLevel(SILGenFunction &SGF);

  void visitSourceFile(SourceFile *SF);
  void visitDecl(Decl *D) {}
  void visitNominalTypeDecl(NominalTypeDecl *NTD);
  void visitExtensionDecl(ExtensionDecl *ED);
  void visitAbstractFunctionDecl(AbstractFunctionDecl *AFD);
  void visitAbstractStorageDecl(AbstractStorageDecl *ASD);
  void visitTopLevelCodeDecl(TopLevelCodeDecl *TD);

private:
  /// The `SILGenFunction` where toplevel code is emitted
  SILGenFunction &SGF;

  /// Walks type declarations to scan for instances where unitialized global
  /// variables are captured by function declarations and emits
  /// `mark_function_escape` SIL instructions for these escape points as needed
  class TypeVisitor : public TypeMemberVisitor<TypeVisitor> {
  public:
    /// Emit `mark_function_escape` SIL instructions into `SGF` for encountered
    /// escape points.
    TypeVisitor(SILGenFunction &SGF);
    void visitDecl(Decl *D) {}
    void emit(IterableDeclContext *Ctx);
    virtual void visitPatternBindingDecl(PatternBindingDecl *PD);
    void visitNominalTypeDecl(NominalTypeDecl *ntd);
    void visitAbstractFunctionDecl(AbstractFunctionDecl *AFD);
    void visitAbstractStorageDecl(AbstractStorageDecl *ASD);
    virtual ~TypeVisitor() {}

  private:
    SILGenFunction &SGF;
  };
  class ExtensionVisitor : public TypeVisitor {
  public:
    /// Emit `mark_function_escape` SIL instructions into `SGF` for encountered
    /// escape points.
    ExtensionVisitor(SILGenFunction &SGF);
    void visitPatternBindingDecl(PatternBindingDecl *PD) override;
  };
};

} // namespace Lowering

} // namespace swift

#endif
