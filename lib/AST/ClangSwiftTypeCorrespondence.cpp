//- ClangSwiftTypeCorrespondence.cpp - Relations between Clang & Swift types -//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// See description in ClangSwiftTypeCorrespondence.h.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ClangSwiftTypeCorrespondence.h"
#include "swift/Basic/Assertions.h"
#include "clang/AST/Type.h"

bool swift::canImportAsOptional(const clang::Type *type) {
  // Note: this mimics ImportHint::canImportAsOptional.

  // Includes CoreFoundation types such as CFStringRef (== struct CFString *).
  return type && (type->isPointerType()
                  || type->isBlockPointerType()
                  || type->isObjCObjectPointerType());
}
