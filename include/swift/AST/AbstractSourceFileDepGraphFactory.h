//===----- AbstractSourceFileDepGraphFactory.h ----------------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_AST_SOURCE_FILE_DEP_GRAPH_CONSTRUCTOR_H
#define SWIFT_AST_SOURCE_FILE_DEP_GRAPH_CONSTRUCTOR_H

#include "swift/AST/Decl.h"
#include "swift/AST/DeclContext.h"
#include "swift/AST/FineGrainedDependencies.h"
#include "llvm/ADT/Optional.h"
#include "llvm/Support/VirtualOutputBackend.h"

namespace swift {
class DiagnosticEngine;
namespace fine_grained_dependencies {

/// Abstract class for building a \c SourceFileDepGraph from either a real
/// \c SourceFile or a unit test
class AbstractSourceFileDepGraphFactory {
protected:
  /// If there was an error, cannot get accurate info.
  const bool hadCompilationError;

  /// The name of the swiftDeps file.
  const std::string swiftDeps;

  /// The fingerprint of the whole file
  Fingerprint fileFingerprint;

  /// For debugging
  const bool emitDotFileAfterConstruction;

  DiagnosticEngine &diags;

  /// OutputBackend.
  llvm::vfs::OutputBackend &backend;

  /// Graph under construction
  SourceFileDepGraph g;

public:
  /// Expose this layer to enable faking up a constructor for testing.
  /// See the instance variable comments for explanation.
  AbstractSourceFileDepGraphFactory(bool hadCompilationError,
                                    StringRef swiftDeps,
                                    Fingerprint fileFingerprint,
                                    bool emitDotFileAfterConstruction,
                                    DiagnosticEngine &diags,
                                    llvm::vfs::OutputBackend &outputBackend);

  virtual ~AbstractSourceFileDepGraphFactory() = default;

  /// Create a SourceFileDepGraph.
  SourceFileDepGraph construct();

private:
  void addSourceFileNodesToGraph();

  /// Add the "provides" nodes when mocking up a graph
  virtual void addAllDefinedDecls() = 0;

  /// Add the "depends" nodes and arcs when mocking a graph
  virtual void addAllUsedDecls() = 0;

protected:
  /// Given an array of Decls or pairs of them in \p declsOrPairs
  /// create node pairs for context and name
  template <NodeKind kind, typename ContentsT>
  void addAllDefinedDeclsOfAGivenType(std::vector<ContentsT> &contentsVec) {
    for (const auto &declOrPair : contentsVec) {
      auto fp =
          AbstractSourceFileDepGraphFactory::getFingerprintIfAny(declOrPair);
      auto key = DependencyKey::Builder{kind, DeclAspect::interface}
                    .withContext(declOrPair)
                    .withName(declOrPair)
                    .build();
      addADefinedDecl(key, fp);
    }
  }

  /// Add an pair of interface, implementation nodes to the graph, which
  /// represent some \c Decl defined in this source file. \param key the
  /// interface key of the pair
  void addADefinedDecl(const DependencyKey &key,
                       llvm::Optional<Fingerprint> fingerprint);

  void addAUsedDecl(const DependencyKey &def, const DependencyKey &use);

  /// Add an external dependency node to the graph. If the provided fingerprint
  /// is not \c None, it is added to the def key.
  void
  addAnExternalDependency(const DependencyKey &def, const DependencyKey &use,
                          llvm::Optional<Fingerprint> dependencyFingerprint);

  static llvm::Optional<Fingerprint>
  getFingerprintIfAny(std::pair<const NominalTypeDecl *, const ValueDecl *>) {
    return llvm::None;
  }

  static llvm::Optional<Fingerprint> getFingerprintIfAny(const Decl *d) {
    if (const auto *idc = dyn_cast<IterableDeclContext>(d)) {
      return idc->getBodyFingerprint();
    }
    return llvm::None;
  }
};

} // namespace fine_grained_dependencies
} // namespace swift

#endif // SWIFT_AST_SOURCE_FILE_DEP_GRAPH_CONSTRUCTOR_H
