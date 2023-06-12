//===--- MacroDiscriminatorContext.h - Macro Discriminators -----*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_AST_MACRO_DISCRIMINATOR_CONTEXT_H
#define SWIFT_AST_MACRO_DISCRIMINATOR_CONTEXT_H

#include "swift/AST/Decl.h"
#include "swift/AST/Expr.h"
#include "llvm/ADT/PointerUnion.h"

namespace swift {

/// Describes the context of a macro expansion for the purpose of
/// computing macro expansion discriminators.
struct MacroDiscriminatorContext
    : public llvm::PointerUnion<DeclContext *, FreestandingMacroExpansion *> {
  using PointerUnion::PointerUnion;

  static MacroDiscriminatorContext getParentOf(FreestandingMacroExpansion *expansion);
  static MacroDiscriminatorContext getParentOf(
      SourceLoc loc, DeclContext *origDC
  );

  /// Return the innermost declaration context that is suitable for
  /// use in identifying a macro.
  static DeclContext *getInnermostMacroContext(DeclContext *dc);
};

}

#endif // SWIFT_AST_MACRO_DISCRIMINATOR_CONTEXT_H
