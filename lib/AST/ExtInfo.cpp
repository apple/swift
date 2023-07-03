//===--- ExtInfo.cpp - Extended information for function types ------------===//
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
//
//  This file implements ASTExtInfo, SILExtInfo and related classes.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ClangModuleLoader.h"
#include "swift/AST/ExtInfo.h"

#include "clang/AST/Type.h"

#include "llvm/ADT/Optional.h"

namespace swift {

// MARK: - ClangTypeInfo

bool operator==(ClangTypeInfo lhs, ClangTypeInfo rhs) {
  if (lhs.type == rhs.type)
    return true;
  if (lhs.type && rhs.type)
    return lhs.type->getCanonicalTypeInternal() ==
           rhs.type->getCanonicalTypeInternal();
  return false;
}

bool operator!=(ClangTypeInfo lhs, ClangTypeInfo rhs) {
  return !(lhs == rhs);
}

ClangTypeInfo ClangTypeInfo::getCanonical() const {
  if (!type)
    return ClangTypeInfo();
  return ClangTypeInfo(type->getCanonicalTypeInternal().getTypePtr());
}

void ClangTypeInfo::printType(ClangModuleLoader *cml,
                              llvm::raw_ostream &os) const {
  cml->printClangType(type, os);
}

void ClangTypeInfo::dump(llvm::raw_ostream &os,
                         const clang::ASTContext &ctx) const {
  if (type) {
    type->dump(os, ctx);
  } else {
    os << "<nullptr>";
  }
}

// MARK: - UnexpectedClangTypeError

llvm::Optional<UnexpectedClangTypeError>
UnexpectedClangTypeError::checkClangType(SILFunctionTypeRepresentation silRep,
                                         const clang::Type *type,
                                         bool expectNonnullForCOrBlock,
                                         bool expectCanonical) {
#ifdef NDEBUG
  return llvm::None;
#else
  bool isBlock = true;
  switch (silRep) {
  case SILFunctionTypeRepresentation::CXXMethod:
  case SILFunctionTypeRepresentation::CFunctionPointer:
      isBlock = false;
      LLVM_FALLTHROUGH;
  case SILFunctionTypeRepresentation::Block: {
    if (!type) {
      if (expectNonnullForCOrBlock)
        return {{Kind::NullForCOrBlock, type}};
      return llvm::None;
    }
    if (expectCanonical && !type->isCanonicalUnqualified())
      return {{Kind::NonCanonical, type}};
    if (isBlock && !type->isBlockPointerType())
      return {{Kind::NotBlockPointer, type}};
    if (!isBlock && !(type->isFunctionPointerType()
                      || type->isFunctionReferenceType()))
      return {{Kind::NotFunctionPointerOrReference, type}};
    return llvm::None;
  }
  default: {
    if (type)
      return {{Kind::NonnullForNonCOrBlock, type}};
    return llvm::None;
  }
  }
#endif
}

void UnexpectedClangTypeError::dump() {
  auto &e = llvm::errs();
  using Kind = UnexpectedClangTypeError::Kind;
  switch (errorKind) {
  case Kind::NullForCOrBlock: {
    e << "Expected non-null Clang type for @convention(c)/@convention(block)"
      << " function but found nullptr.";
    return;
  }
  case Kind::NonnullForNonCOrBlock: {
    e << ("Expected null Clang type for non-@convention(c),"
          " non-@convention(block) function but found:\n");
    type->dump();
    return;
  }
  case Kind::NotBlockPointer: {
    e << ("Expected block pointer type for @convention(block) function but"
          " found:\n");
    type->dump();
    return;
  }
  case Kind::NotFunctionPointerOrReference: {
    e << ("Expected function pointer/reference type for @convention(c) function"
          " but found:\n");
    type->dump();
    return;
  }
  case Kind::NonCanonical: {
    e << "Expected canonicalized Clang type but found:\n";
    type->dump();
    return;
  }
  }
  llvm_unreachable("Unhandled case for UnexpectedClangTypeError");
}

// [NOTE: ExtInfo-Clang-type-invariant]
// At the SIL level, all @convention(c) and @convention(block) function types
// are expected to carry a ClangTypeInfo. This is not enforced at the AST level
// because we may synthesize types which are not convertible to Clang types.
// 1. Type errors: If we have a type error, we may end up generating (say) a
//    @convention(c) function type that has an ErrorType as a parameter.
// 2. Bridging: The representation can change during bridging. For example, an
//    @convention(swift) function can be bridged to an @convention(block)
//    function. Since this happens during SILGen, we may see a "funny" type
//    like @convention(c) () -> @convention(swift) () -> () at the AST level.

// MARK: - ASTExtInfoBuilder

void ASTExtInfoBuilder::checkInvariants() const {
  // See [NOTE: ExtInfo-Clang-type-invariant]
  if (auto error = UnexpectedClangTypeError::checkClangType(
          getSILRepresentation(), clangTypeInfo.getType(), false, false)) {
    error.value().dump();
    llvm_unreachable("Ill-formed ASTExtInfoBuilder.");
  }
}

// MARK: - ASTExtInfo

ASTExtInfo ASTExtInfoBuilder::build() const {
  checkInvariants();
  return ASTExtInfo(*this);
}

// MARK: - SILExtInfoBuilder

void SILExtInfoBuilder::checkInvariants() const {
  // See [NOTE: ExtInfo-Clang-type-invariant]
  // [FIXME: Clang-type-plumbing] Strengthen check when UseClangFunctionTypes
  // is removed.
  if (auto error = UnexpectedClangTypeError::checkClangType(
          getRepresentation(), clangTypeInfo.getType(), false, true)) {
    error.value().dump();
    llvm_unreachable("Ill-formed SILExtInfoBuilder.");
  }
}

SILExtInfo SILExtInfoBuilder::build() const {
  checkInvariants();
  return SILExtInfo(*this);
}

// MARK: - SILExtInfo

llvm::Optional<UnexpectedClangTypeError> SILExtInfo::checkClangType() const {
  return UnexpectedClangTypeError::checkClangType(
      getRepresentation(), getClangTypeInfo().getType(), true, true);
}

} // end namespace swift
