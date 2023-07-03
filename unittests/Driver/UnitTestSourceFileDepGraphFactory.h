//===----- UnitTestSourceFileDepGraphFactory.h ------------------*- C++ -*-===//
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
#ifndef UnitTestSourceFileDepGraphFactory_h
#define UnitTestSourceFileDepGraphFactory_h

#include "swift/AST/AbstractSourceFileDepGraphFactory.h"
#include "llvm/Support/VirtualOutputBackend.h"

namespace swift {
namespace fine_grained_dependencies {
namespace mocking_fine_grained_dependency_graphs {

using DependencyDescriptions =
    std::unordered_multimap<NodeKind, std::vector<std::string>>;

class UnitTestSourceFileDepGraphFactory
    : public AbstractSourceFileDepGraphFactory {
  const DependencyDescriptions dependencyDescriptions;

public:
  UnitTestSourceFileDepGraphFactory(
      bool hadCompilationError, StringRef swiftDeps,
      Fingerprint fileFingerprint, bool emitDotFileAfterConstruction,
      const DependencyDescriptions &dependencyDescriptions,
      DiagnosticEngine &diags, llvm::vfs::OutputBackend &backend)
      : AbstractSourceFileDepGraphFactory(
            hadCompilationError, swiftDeps, fileFingerprint,
            emitDotFileAfterConstruction, diags, backend),
        dependencyDescriptions(dependencyDescriptions) {}

  ~UnitTestSourceFileDepGraphFactory() override = default;

private:
  void addAllDefinedDecls() override;
  void addAllUsedDecls() override;

  /// For brevity, unit tests specify dependencies by NodeKind,
  /// but for processing, the kind is needed for each entry.
  void forEachEntry(function_ref<void(NodeKind kind, StringRef entry)> fn);

  static const char *defUseSeparator;
  static bool isADefinedDecl(StringRef s);

  void addADefinedDecl(StringRef s, NodeKind kind);
  void addAUsedDecl(StringRef s, NodeKind kind);

  llvm::Optional<std::pair<DependencyKey, DependencyKey>>
  parseAUsedDecl(StringRef s, NodeKind);

  /// Parse and return an interface \c DependencyKey
  llvm::Optional<DependencyKey> parseADefinedDecl(StringRef s, NodeKind,
                                                  DeclAspect);

  DependencyKey computeUseKey(StringRef s, bool isCascadingUse);

  /// Return true if when the name appears in a unit test, it represents a
  /// context, not a baseName. Return false if a single name is a baseName,
  /// without context Return None if there should be two names
  static llvm::Optional<bool> singleNameIsContext(NodeKind kind);

  static constexpr char nameContextSeparator = ',';

  static constexpr char fingerprintSeparator = '@';

  static std::string parseContext(const StringRef s, const NodeKind kind);

  static std::string parseName(const StringRef s, const NodeKind kind);
};

} // namespace mocking_fine_grained_dependency_graphs
} // namespace fine_grained_dependencies
} // namespace swift

#endif /* UnitTestSourceFileDepGraphFactory_h */
