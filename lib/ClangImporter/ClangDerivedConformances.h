//===--- ClangDerivedConformances.h -----------------------------*- C++ -*-===//
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

#ifndef SWIFT_CLANG_DERIVED_CONFORMANCES_H
#define SWIFT_CLANG_DERIVED_CONFORMANCES_H

#include "ImporterImpl.h"
#include "swift/AST/ASTContext.h"

namespace swift {

bool isIterator(const clang::CXXRecordDecl *clangDecl);

bool isUnsafeStdMethod(const clang::CXXMethodDecl *methodDecl);

/// If the decl is a C++ input iterator, synthesize a conformance to the
/// UnsafeCxxInputIterator protocol, which is defined in the Cxx module.
void conformToCxxIteratorIfNeeded(ClangImporter::Implementation &impl,
                                  NominalTypeDecl *decl,
                                  const clang::CXXRecordDecl *clangDecl);

/// If the decl is an instantiation of C++ `std::optional`, synthesize a
/// conformance to CxxOptional protocol, which is defined in the Cxx module.
void conformToCxxOptionalIfNeeded(ClangImporter::Implementation &impl,
                                  NominalTypeDecl *decl,
                                  const clang::CXXRecordDecl *clangDecl);

/// If the decl is a C++ sequence, synthesize a conformance to the CxxSequence
/// protocol, which is defined in the Cxx module.
void conformToCxxSequenceIfNeeded(ClangImporter::Implementation &impl,
                                  NominalTypeDecl *decl,
                                  const clang::CXXRecordDecl *clangDecl);

/// If the decl is an instantiation of C++ `std::set`, `std::unordered_set` or
/// `std::multiset`, synthesize a conformance to CxxSet, which is defined in the
/// Cxx module.
void conformToCxxSetIfNeeded(ClangImporter::Implementation &impl,
                             NominalTypeDecl *decl,
                             const clang::CXXRecordDecl *clangDecl);

/// If the decl is an instantiation of C++ `std::pair`, synthesize a conformance
/// to CxxPair, which is defined in the Cxx module.
void conformToCxxPairIfNeeded(ClangImporter::Implementation &impl,
                              NominalTypeDecl *decl,
                              const clang::CXXRecordDecl *clangDecl);

/// If the decl is an instantiation of C++ `std::map` or `std::unordered_map`,
/// synthesize a conformance to CxxDictionary, which is defined in the Cxx module.
void conformToCxxDictionaryIfNeeded(ClangImporter::Implementation &impl,
                                    NominalTypeDecl *decl,
                                    const clang::CXXRecordDecl *clangDecl);

/// If the decl is an instantiation of C++ `std::vector`, synthesize a
/// conformance to CxxVector, which is defined in the Cxx module.
void conformToCxxVectorIfNeeded(ClangImporter::Implementation &impl,
                                NominalTypeDecl *decl,
                                const clang::CXXRecordDecl *clangDecl);

} // namespace swift

#endif // SWIFT_CLANG_DERIVED_CONFORMANCES_H
