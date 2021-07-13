//===--- SynthesizedFileUnit.h - A synthesized file unit --------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_AST_SYNTHESIZEDFILEUNIT_H
#define SWIFT_AST_SYNTHESIZEDFILEUNIT_H

#include "swift/AST/FileUnit.h"
#include "swift/Basic/Debug.h"

namespace swift {

class SourceFile;

/// A container for synthesized declarations, attached to a `SourceFile`.
///
/// Currently, only module-level synthesized declarations are supported.
class SynthesizedFileUnit final : public FileUnit {
  /// The parent source file.
  SourceFile &SF;

  /// Synthesized top level declarations.
  llvm::SmallVector<Decl *, 4> TopLevelDecls;

  /// A unique identifier representing this file; used to mark private decls
  /// within the file to keep them from conflicting with other files in the
  /// same module.
  mutable Identifier PrivateDiscriminator;

public:
  SynthesizedFileUnit(SourceFile &SF);
  ~SynthesizedFileUnit() = default;

  /// Returns the parent source file.
  SourceFile &getSourceFile() const { return SF; }

  /// Prepends a declaration to the top-level decls list.
  ///
  /// FIXME: This entrypoint exists to support LLDB. Calls to this function are
  /// always a mistake, and additional uses should not be added.
  ///
  /// See rdar://58355191
  void prependTopLevelDecl(Decl *d) {
    TopLevelDecls.insert(TopLevelDecls.begin(), d);
  }

  /// Add a synthesized top-level declaration.
  void addTopLevelDecl(Decl *D) { TopLevelDecls.push_back(D); }

  virtual void lookupValue(DeclName name, NLKind lookupKind,
                           SmallVectorImpl<ValueDecl *> &result) const override;

  void lookupObjCMethods(
      ObjCSelector selector,
      SmallVectorImpl<AbstractFunctionDecl *> &results) const override;

  Identifier getDiscriminatorForPrivateValue(const ValueDecl *D) const override;

  void getTopLevelDecls(SmallVectorImpl<Decl*> &results) const override;

  ArrayRef<Decl *> getTopLevelDecls() const {
    return TopLevelDecls;
  };

  static bool classof(const FileUnit *file) {
    return file->getKind() == FileUnitKind::Synthesized;
  }
  static bool classof(const DeclContext *DC) {
    return isa<FileUnit>(DC) && classof(cast<FileUnit>(DC));
  }
};

} // namespace swift

#endif // SWIFT_AST_SYNTHESIZEDFILEUNIT_H
