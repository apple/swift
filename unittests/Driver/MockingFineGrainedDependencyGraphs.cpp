//===--------------- MockingFineGrainedDependencyGraphs.cpp ---------------===//
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

#include "MockingFineGrainedDependencyGraphs.h"
#include "swift/AST/AbstractSourceFileDepGraphFactory.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/Basic/ReferenceDependencyKeys.h"
#include "swift/Basic/SourceManager.h"
#include "llvm/Support/VirtualOutputBackends.h"

#include <array>
#include <unordered_map>

using namespace swift;
using namespace fine_grained_dependencies;
using namespace mocking_fine_grained_dependency_graphs;

void mocking_fine_grained_dependency_graphs::simulateLoad(
    ModuleDepGraph &g, const driver::Job *cmd,
    const DependencyDescriptions &dependencyDescriptions,
    llvm::Optional<Fingerprint> interfaceHashIfNonEmpty,
    const bool hadCompilationError) {
  const auto changes = getChangesForSimulatedLoad(
      g, cmd, dependencyDescriptions, interfaceHashIfNonEmpty,
      hadCompilationError);
  assert(changes && "simulated load should always succeed");
}

ModuleDepGraph::Changes
mocking_fine_grained_dependency_graphs::getChangesForSimulatedLoad(
    ModuleDepGraph &g, const driver::Job *cmd,
    const DependencyDescriptions &dependencyDescriptions,
    llvm::Optional<Fingerprint> interfaceHashIfNonEmpty,
    const bool hadCompilationError) {
  auto swiftDeps =
    cmd->getOutput().getAdditionalOutputForType(file_types::TY_SwiftDeps).str();
  auto swiftDepsFingerprint =
    swift::mockFingerprintFromString(swiftDeps).value();
  auto interfaceHash = interfaceHashIfNonEmpty.value_or(swiftDepsFingerprint);

  SourceManager sm;
  DiagnosticEngine diags(sm);
  llvm::vfs::OnDiskOutputBackend backend;

  auto sfdg =
      UnitTestSourceFileDepGraphFactory(
          hadCompilationError, swiftDeps, interfaceHash,
          g.emitFineGrainedDependencyDotFileAfterEveryImport,
          dependencyDescriptions, diags, backend)
          .construct();

  return g.loadFromSourceFileDepGraph(cmd, sfdg, diags);
}

std::vector<const driver::Job *>
mocking_fine_grained_dependency_graphs::simulateReload(
    ModuleDepGraph &g, const driver::Job *cmd,
    const DependencyDescriptions &dependencyDescriptions,
    llvm::Optional<Fingerprint> interfaceHashIfNonEmpty,
    const bool hadCompilationError) {
  const auto changedNodes = getChangesForSimulatedLoad(
      g, cmd, dependencyDescriptions, interfaceHashIfNonEmpty,
      hadCompilationError);
  if (!changedNodes)
    return g.getAllJobs();
  return g.findJobsToRecompileWhenNodesChange(changedNodes.value());
}

LLVM_ATTRIBUTE_UNUSED
std::vector<const driver::Job *>
mocking_fine_grained_dependency_graphs::printJobsForDebugging(
    const std::vector<const driver::Job *> &jobs) {
  llvm::errs() << "\nprintForDebugging: ";
  for (auto *j : jobs) {
    const auto swiftDeps =
        j->getOutput().getAdditionalOutputForType(file_types::TY_SwiftDeps);
    assert(!swiftDeps.empty());
    llvm::errs() << "job" << swiftDeps << " ";
  }
  llvm::errs() << "\n";
  return jobs;
}
