//===--- CFTypeInfo.cpp - Information about CF types  ---------------------===//
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
//
// This file provides support for reasoning about CF types
//
//===----------------------------------------------------------------------===//

#include "CFTypeInfo.h"
#include "ImporterImpl.h"

using namespace swift;
using namespace importer;

namespace {
  // Quasi-lexicographic order: string length first, then string data.
  // Since we don't care about the actual length, we can use this, which
  // lets us ignore the string data a larger proportion of the time.
  struct SortByLengthComparator {
    bool operator()(StringRef lhs, StringRef rhs) const {
      return (lhs.size() < rhs.size() ||
              (lhs.size() == rhs.size() && lhs < rhs));
    }
  };
} // end anonymous namespace

/// The list of known CF types.  We use 'constexpr' to verify that this is
/// emitted as a constant.  Note that this is expected to be sorted in
/// quasi-lexicographic order.
static constexpr const llvm::StringLiteral KnownCFTypes[] = {
#define CF_TYPE(NAME) #NAME,
#define NON_CF_TYPE(NAME)
#include "SortedCFDatabase.def"
};
const size_t NumKnownCFTypes = sizeof(KnownCFTypes) / sizeof(*KnownCFTypes);

/// Maintain a set of known CF types.
bool CFPointeeInfo::isKnownCFTypeName(StringRef name) {
  return std::binary_search(KnownCFTypes, KnownCFTypes + NumKnownCFTypes,
                            name, SortByLengthComparator());
}

/// Classify a potential CF typedef.
CFPointeeInfo
CFPointeeInfo::classifyTypedef(const clang::TypedefNameDecl *typedefDecl) {
  clang::QualType type = typedefDecl->getUnderlyingType();

  if (auto elaborated = type->getAs<clang::ElaboratedType>())
    type = elaborated->desugar();

  if (auto subTypedef = type->getAs<clang::TypedefType>()) {
    if (classifyTypedef(subTypedef->getDecl()))
      return forTypedef(subTypedef->getDecl());
    return forInvalid();
  }

  if (auto ptr = type->getAs<clang::PointerType>()) {
    auto pointee = ptr->getPointeeType();

    // Must be 'const' or nothing.
    clang::Qualifiers quals = pointee.getQualifiers();
    bool isConst = quals.hasConst();
    quals.removeConst();
    if (quals.empty()) {
      if (auto record = pointee->getAs<clang::RecordType>()) {
        auto recordDecl = record->getDecl();
        if (recordDecl->hasAttr<clang::ObjCBridgeAttr>() ||
            recordDecl->hasAttr<clang::ObjCBridgeMutableAttr>() ||
            recordDecl->hasAttr<clang::ObjCBridgeRelatedAttr>() ||
            isKnownCFTypeName(typedefDecl->getName())) {
          return forRecord(isConst, record->getDecl());
        }
      } else if (pointee->isVoidType()) {
        if (typedefDecl->hasAttr<clang::ObjCBridgeAttr>() ||
            isKnownCFTypeName(typedefDecl->getName())) {
          return isConst ? forConstVoid() : forVoid();
        }
      }
    }
  }

  return forInvalid();
}

bool importer::isCFTypeDecl(
       const clang::TypedefNameDecl *Decl) {
  if (CFPointeeInfo::classifyTypedef(Decl))
    return true;
  return false;
}

StringRef importer::getCFTypeName(
            const clang::TypedefNameDecl *decl) {
  if (auto pointee = CFPointeeInfo::classifyTypedef(decl)) {
    auto name = decl->getName();
    if (pointee.isRecord() || pointee.isTypedef())
      if (name.endswith(SWIFT_CFTYPE_SUFFIX))
        return name.drop_back(strlen(SWIFT_CFTYPE_SUFFIX));

    return name;
  }

  return "";
}
