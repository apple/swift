//===--- CFTypeInfo.h - Information about CF types  -------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2016 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file provides support for reasoning about CF types
//
//===----------------------------------------------------------------------===//
#ifndef SWIFT_IMPORTER_CFTYPEINFO_H
#define SWIFT_IMPORTER_CFTYPEINFO_H 

#include "llvm/ADT/PointerUnion.h"

namespace clang {
  class RecordDecl;
  class TypedefNameDecl;
}

namespace swift {
namespace importer {

class CFPointeeInfo {
  bool IsValid;
  bool IsConst;
  llvm::PointerUnion<const clang::RecordDecl *, const clang::TypedefNameDecl *>
      Decl;
  CFPointeeInfo() = default;

  static CFPointeeInfo forRecord(bool isConst, const clang::RecordDecl *decl) {
    assert(decl);
    CFPointeeInfo info;
    info.IsValid = true;
    info.IsConst = isConst;
    info.Decl = decl;
    return info;
  }

  static CFPointeeInfo forTypedef(const clang::TypedefNameDecl *decl) {
    assert(decl);
    CFPointeeInfo info;
    info.IsValid = true;
    info.IsConst = false;
    info.Decl = decl;
    return info;
  }

  static CFPointeeInfo forConstVoid() {
    CFPointeeInfo info;
    info.IsValid = true;
    info.IsConst = true;
    info.Decl = nullptr;
    return info;
  }

  static CFPointeeInfo forVoid() {
    CFPointeeInfo info;
    info.IsValid = true;
    info.IsConst = false;
    info.Decl = nullptr;
    return info;
  }

  static CFPointeeInfo forInvalid() {
    CFPointeeInfo info;
    info.IsValid = false;
    return info;
  }

public:
  static CFPointeeInfo classifyTypedef(const clang::TypedefNameDecl *decl);

  bool isValid() const { return IsValid; }
  explicit operator bool() const { return isValid(); }

  bool isConst() const { return IsConst; }

  bool isVoid() const {
    assert(isValid());
    return Decl.isNull();
  }

  bool isRecord() const {
    assert(isValid());
    return !Decl.isNull() && Decl.is<const clang::RecordDecl *>();
  }
  const clang::RecordDecl *getRecord() const {
    assert(isRecord());
    return Decl.get<const clang::RecordDecl *>();
  }

  bool isTypedef() const {
    assert(isValid());
    return !Decl.isNull() && Decl.is<const clang::TypedefNameDecl *>();
  }
  const clang::TypedefNameDecl *getTypedef() const {
    assert(isTypedef());
    return Decl.get<const clang::TypedefNameDecl *>();
  }
};

/// Determine whether this typedef is a CF type.
bool isCFTypeDecl(const clang::TypedefNameDecl *Decl);

/// Determine the imported CF type for the given typedef-name, or the empty
/// string if this is not an imported CF type name.
llvm::StringRef getCFTypeName(const clang::TypedefNameDecl *decl);
}
}

#endif // SWIFT_IMPORTER_CFTYPEINFO_H
