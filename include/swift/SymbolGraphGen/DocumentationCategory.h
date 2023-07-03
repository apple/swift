//===--- DocumentationCategory.h - Accessors for @_documentation ----------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SYMBOLGRAPHGEN_DOCUMENTATIONCATEGORY_H
#define SWIFT_SYMBOLGRAPHGEN_DOCUMENTATIONCATEGORY_H

#include "swift/AST/Decl.h"

#include "llvm/Support/Compiler.h"

namespace swift {
namespace symbolgraphgen {

LLVM_ATTRIBUTE_USED
static StringRef documentationMetadataForDecl(const Decl *D) {
  if (!D) return {};

  if (const auto *DC = D->getAttrs().getAttribute<DocumentationAttr>()) {
    return DC->Metadata;
  }

  return {};
}

LLVM_ATTRIBUTE_USED
static llvm::Optional<AccessLevel>
documentationVisibilityForDecl(const Decl *D) {
  if (!D)
    return llvm::None;

  if (const auto *DC = D->getAttrs().getAttribute<DocumentationAttr>()) {
    return DC->Visibility;
  }

  return llvm::None;
}

} // namespace symbolgraphgen
} // namespace swift

#endif // SWIFT_SYMBOLGRAPHGEN_DOCUMENTATIONCATEGORY_H
