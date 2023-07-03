//===-------- AbstractSourceFileDepGraphFactory.cpp -----------------------===//
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

#include "swift/AST/AbstractSourceFileDepGraphFactory.h"

// may not all be needed
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/AST/FileSystem.h"
#include "swift/AST/FineGrainedDependencies.h"
#include "swift/Basic/FileSystem.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/ReferenceDependencyKeys.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/VirtualOutputBackend.h"
#include "llvm/Support/YAMLParser.h"

using namespace swift;
using namespace fine_grained_dependencies;

//==============================================================================
// MARK: AbstractSourceFileDepGraphFactory - client interface
//==============================================================================

AbstractSourceFileDepGraphFactory::AbstractSourceFileDepGraphFactory(
    bool hadCompilationError, StringRef swiftDeps, Fingerprint fileFingerprint,
    bool emitDotFileAfterConstruction, DiagnosticEngine &diags,
    llvm::vfs::OutputBackend &backend)
    : hadCompilationError(hadCompilationError), swiftDeps(swiftDeps.str()),
      fileFingerprint(fileFingerprint),
      emitDotFileAfterConstruction(emitDotFileAfterConstruction), diags(diags),
      backend(backend) {}

SourceFileDepGraph AbstractSourceFileDepGraphFactory::construct() {
  addSourceFileNodesToGraph();
  if (!hadCompilationError) {
    addAllDefinedDecls();
    addAllUsedDecls();
  }
  assert(g.verify());
  if (emitDotFileAfterConstruction)
    g.emitDotFile(backend, swiftDeps, diags);
  return std::move(g);
}

//==============================================================================
// MARK: AbstractSourceFileDepGraphFactory - adding a defined or used Decl
//==============================================================================
void AbstractSourceFileDepGraphFactory::addSourceFileNodesToGraph() {
  g.findExistingNodePairOrCreateAndAddIfNew(
      DependencyKey::createKeyForWholeSourceFile(DeclAspect::interface,
                                                 swiftDeps),
      Fingerprint{fileFingerprint});
}

void AbstractSourceFileDepGraphFactory::addADefinedDecl(
    const DependencyKey &interfaceKey,
    llvm::Optional<Fingerprint> fingerprint) {

  auto nodePair =
      g.findExistingNodePairOrCreateAndAddIfNew(interfaceKey, fingerprint);
  // Since the current type fingerprints only include tokens in the body,
  // when the interface hash changes, it is possible that the type in the
  // file has changed.
  g.addArc(g.getSourceFileNodePair().getInterface(), nodePair.getInterface());
}

void AbstractSourceFileDepGraphFactory::addAUsedDecl(
    const DependencyKey &defKey, const DependencyKey &useKey) {
  auto *defNode = g.findExistingNodeOrCreateIfNew(defKey, llvm::None,
                                                  false /* = !isProvides */);

  // If the depended-upon node is defined in this file, then don't
  // create an arc to the user, when the user is the whole file.
  // Otherwise, if the defNode's type-body fingerprint changes,
  // the whole file will be marked as dirty, losing the benefit of the
  // fingerprint.

  //  if (defNode->getIsProvides() &&
  //      useKey.getKind() == NodeKind::sourceFileProvide)
  //    return;

  // Turns out the above three lines cause miscompiles, so comment them out
  // for now. We might want them back if we can change the inputs to this
  // function to be more precise.

  // Example of a miscompile:
  // In main.swift
  // func foo(_: Any) { print("Hello Any") }
  //    foo(123)
  // Then add the following line to another file:
  // func foo(_: Int) { print("Hello Int") }
  // Although main.swift needs to get recompiled, the commented-out code below
  // prevents that.

  auto nullableUse = g.findExistingNode(useKey);
  assert(nullableUse.isNonNull() && "Use must be an already-added provides");
  auto *useNode = nullableUse.get();
  assert(useNode->getIsProvides() && "Use (using node) must be a provides");
  g.addArc(defNode, useNode);
}

void AbstractSourceFileDepGraphFactory::addAnExternalDependency(
    const DependencyKey &defKey, const DependencyKey &useKey,
    llvm::Optional<Fingerprint> maybeFP) {
  auto *defNode = g.findExistingNodeOrCreateIfNew(defKey, maybeFP,
                                                  false /* = !isProvides */);

  auto nullableUse = g.findExistingNode(useKey);
  assert(nullableUse.isNonNull() && "Use must be an already-added provides");
  auto *useNode = nullableUse.get();
  assert(useNode->getIsProvides() && "Use (using node) must be a provides");
  g.addArc(defNode, useNode);
}
