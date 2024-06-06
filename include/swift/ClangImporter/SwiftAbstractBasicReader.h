//===- SwiftAbstractBasicReader.h - Clang serialization adapter -*- C++ -*-===//
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
// This file provides an intermediate CRTP class which implements most of
// Clang's AbstractBasicReader interface, paralleling the behavior defined
// in SwiftAbstractBasicWriter.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_CLANGIMPORTER_SWIFTABSTRACTBASICREADER_H
#define SWIFT_CLANGIMPORTER_SWIFTABSTRACTBASICREADER_H

#include "clang/AST/ASTContext.h"
#include "clang/AST/AbstractTypeReader.h"

// This include is required to instantiate the template code in
// AbstractBasicReader.h, i.e. it's a workaround to an include-what-you-use
// violation.
#include "clang/AST/DeclObjC.h"

namespace swift {

/// An implementation of Clang's AbstractBasicReader interface for a Swift
/// datastream-based reader.  This is paired with the AbstractBasicWriter
/// implementation in SwiftAbstractBasicWriter.h.  Note that the general
/// expectation is that the types and declarations involved will have passed
/// a serializability check when this is used for actual deserialization.
///
/// The subclass must implement:
///   uint64_t readUInt64();
///   clang::IdentifierInfo *readIdentifier();
///   clang::Stmt *readStmtRef();
///   clang::Decl *readDeclRef();
template <class Impl>
class DataStreamBasicReader
       : public clang::serialization::DataStreamBasicReader<Impl> {
  using super = clang::serialization::DataStreamBasicReader<Impl>;
public:
  using super::asImpl;
  using super::getASTContext;

  DataStreamBasicReader(clang::ASTContext &ctx) : super(ctx) {}

  /// Perform all the calls necessary to write out the given type.
  clang::QualType readTypeRef() {
    auto kind = clang::Type::TypeClass(asImpl().readUInt64());
    return clang::serialization::AbstractTypeReader<Impl>(asImpl()).read(kind);
  }

  bool readBool() {
    return asImpl().readUInt64() != 0;
  }

  uint32_t readUInt32() {
    return uint32_t(asImpl().readUInt64());
  }

  clang::Selector readSelector() {
    uint64_t numArgsPlusOne = asImpl().readUInt64();

    // The null case.
    if (numArgsPlusOne == 0)
      return clang::Selector();

    unsigned numArgs = unsigned(numArgsPlusOne - 1);
    SmallVector<const clang::IdentifierInfo *, 4> chunks;
    for (unsigned i = 0, e = std::max(numArgs, 1U); i != e; ++i)
      chunks.push_back(asImpl().readIdentifier());

    return getASTContext().Selectors.getSelector(numArgs, chunks.data());
  }

  clang::SourceLocation readSourceLocation() {
    // Always read null.
    return clang::SourceLocation();
  }

  clang::QualType readQualType() {
    clang::Qualifiers quals = asImpl().readQualifiers();
    clang::QualType type = asImpl().readTypeRef();
    return getASTContext().getQualifiedType(type, quals);
  }

  const clang::BTFTypeTagAttr *readBTFTypeTagAttr() {
    llvm::report_fatal_error("Read BTFTypeTagAttr that should never have been"
                             " serialized");
  }

  template<typename T>
  T *readDeclAs() {
    return asImpl().template readDeclAs<T>();
  }
};

}

#endif
