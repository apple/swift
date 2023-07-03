//===--- FineGrainedDependencyGraph.cpp ------------------------------------==//
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

#include "swift/Driver/FineGrainedDependencyDriverGraph.h"
// Next two includes needed for reporting errors opening dot file for writing.
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/AST/FileSystem.h"
#include "swift/Basic/PrettyStackTrace.h"
#include "swift/Basic/ReferenceDependencyKeys.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Basic/Statistic.h"
#include "swift/Demangling/Demangle.h"
#include "swift/Driver/Job.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/VirtualOutputBackend.h"
#include "llvm/Support/YAMLParser.h"
#include "llvm/Support/raw_ostream.h"
#include <unordered_set>

// Definitions for the portion fine-grained dependency system used by the
// driver.

using namespace swift;

using namespace swift::fine_grained_dependencies;
using namespace swift::driver;

//==============================================================================
// MARK: Affordances to unit tests
//==============================================================================

LLVM_ATTRIBUTE_UNUSED
std::vector<const Job *>
ModuleDepGraph::printJobsForDebugging(const std::vector<const Job *> &jobs) {
  llvm::errs() << "\nprintForDebugging: ";
  for (auto *j : jobs) {
    const auto swiftDeps =
        j->getOutput().getAdditionalOutputForType(file_types::TY_SwiftDeps);
    assert(!swiftDeps.empty());
    llvm::errs() << "job" << swiftDeps << ", ";
  }
  llvm::errs() << "\n";
  return jobs;
}
//==============================================================================
// MARK: Interfacing to Compilation
//==============================================================================

ModuleDepGraph::Changes ModuleDepGraph::loadFromPath(const Job *Cmd,
                                                     StringRef path,
                                                     DiagnosticEngine &diags) {
  FrontendStatsTracer tracer(stats, "fine-grained-dependencies-loadFromPath");
  PrettyStackTraceStringAction stackTrace("loading fine-grained dependency graph", path);

  if (driverDotFileBasePath.empty()) {
    driverDotFileBasePath = path;
    llvm::sys::path::remove_filename(driverDotFileBasePath);
    llvm::sys::path::append(driverDotFileBasePath, "driver");
  }

  auto buffer = llvm::MemoryBuffer::getFile(path);
  if (!buffer)
    return llvm::None;
  auto r = loadFromBuffer(Cmd, *buffer.get(), diags);
  assert(path == getSwiftDeps(Cmd) && "Should be reading the job's swiftdeps");
  assert(!r || !nodeMap[path.str()].empty() &&
         "Must have a node for the whole file");
  return r;
}

/// Returns None for error or a set of changed keys
ModuleDepGraph::Changes
ModuleDepGraph::loadFromBuffer(const Job *job, llvm::MemoryBuffer &buffer,
                               DiagnosticEngine &diags) {
  llvm::Optional<SourceFileDepGraph> sourceFileDepGraph =
      SourceFileDepGraph::loadFromBuffer(buffer);
  if (!sourceFileDepGraph)
    return llvm::None;
  return loadFromSourceFileDepGraph(job, sourceFileDepGraph.value(), diags);
}

ModuleDepGraph::Changes ModuleDepGraph::loadFromSourceFileDepGraph(
    const Job *job, const SourceFileDepGraph &sourceFileDepGraph,
    DiagnosticEngine &diags) {
  registerJob(job);
  auto changes = integrate(sourceFileDepGraph, getSwiftDeps(job));

  if (verifyFineGrainedDependencyGraphAfterEveryImport)
    verify();
  if (emitFineGrainedDependencyDotFileAfterEveryImport)
    emitDotFileForJob(diags, job);
  return changes;
}

ModuleDepGraph::Changes ModuleDepGraph::loadFromSwiftModuleBuffer(
    const Job *Cmd, llvm::MemoryBuffer &buffer, DiagnosticEngine &diags) {
  FrontendStatsTracer tracer(
      stats, "fine-grained-dependencies-loadFromSwiftModuleBuffer");
  PrettyStackTraceStringAction stackTrace(
      "loading fine-grained dependency graph from swiftmodule",
      buffer.getBufferIdentifier());

  llvm::Optional<SourceFileDepGraph> sourceFileDepGraph =
      SourceFileDepGraph::loadFromSwiftModuleBuffer(buffer);
  if (!sourceFileDepGraph)
    return llvm::None;
  jobsBySwiftDeps[buffer.getBufferIdentifier().str()] = Cmd;
  auto changes = integrate(*sourceFileDepGraph, buffer.getBufferIdentifier());
  if (verifyFineGrainedDependencyGraphAfterEveryImport)
    verify();
  if (emitFineGrainedDependencyDotFileAfterEveryImport)
    emitDotFileForJob(diags, Cmd);
  return changes;
}

bool ModuleDepGraph::haveAnyNodesBeenTraversedIn(const Job *cmd) const {
  std::string swiftDeps = getSwiftDeps(cmd).str();

  // optimization
  const auto fileKey = DependencyKey::createKeyForWholeSourceFile(
      DeclAspect::interface, swiftDeps);
  if (const auto fileNode = nodeMap.find(swiftDeps, fileKey)) {
    if (fileNode && fileNode.value()->getHasBeenTraced())
      return true;
  }

  bool result = false;
  forEachNodeInJob(swiftDeps, [&](const ModuleDepGraphNode *n) {
    if (n->getHasBeenTraced())
      result = true;
  });
  return result;
}

std::vector<const Job *> ModuleDepGraph::findJobsToRecompileWhenWholeJobChanges(
    const Job *jobToBeRecompiled) {
  std::vector<ModuleDepGraphNode *> allNodesInJob;
  forEachNodeInJob(getSwiftDeps(jobToBeRecompiled),
                   [&](ModuleDepGraphNode *n) { allNodesInJob.push_back(n); });
  return findJobsToRecompileWhenNodesChange(allNodesInJob);
}

std::vector<std::string> ModuleDepGraph::computeSwiftDepsFromNodes(
    ArrayRef<const ModuleDepGraphNode *> nodes) const {
  llvm::StringSet<> swiftDepsOfNodes;
  for (const ModuleDepGraphNode *n : nodes) {
    if (!n->getIsProvides())
      continue;
    const std::string &swiftDeps = n->getSwiftDepsOfProvides();
    swiftDepsOfNodes.insert(swiftDeps);
  }
  std::vector<std::string> swiftDepsVec;
  for (const auto &entry : swiftDepsOfNodes)
    swiftDepsVec.push_back(entry.getKey().str());
  return swiftDepsVec;
}

std::vector<const Job *> ModuleDepGraph::jobsContaining(
    ArrayRef<const ModuleDepGraphNode *> nodes) const {
  std::vector<const Job *> jobs;
  for (StringRef swiftDeps : computeSwiftDepsFromNodes(nodes))
    jobs.push_back(getJob(swiftDeps.str()));
  return jobs;
}

void ModuleDepGraph::registerJob(const Job *job) {
  // No need to create any nodes; that will happen when the swiftdeps file is
  // read. Just record the correspondence.
  jobsBySwiftDeps.insert(std::make_pair(getSwiftDeps(job).str(), job));
}

std::vector<const Job *> ModuleDepGraph::getAllJobs() const {
  std::vector<const Job *> jobs;
  for (auto const &entry : jobsBySwiftDeps)
    jobs.push_back(entry.second);
  return jobs;
}

std::vector<StringRef> ModuleDepGraph::getExternalDependencies() const {
  return std::vector<StringRef>(externalDependencies.begin(),
                                externalDependencies.end());
}

// Add every (swiftdeps) use of the external dependency to foundJobs.
// Can return duplicates, but it doesn't break anything, and they will be
// canonicalized later.
std::vector<const Job *> ModuleDepGraph::findExternallyDependentUntracedJobs(
    StringRef externalDependency) {
  FrontendStatsTracer tracer(
      stats, "fine-grained-dependencies-findExternallyDependentUntracedJobs");
  std::vector<const Job *> foundJobs;
  forEachUntracedJobDirectlyDependentOnExternalSwiftDeps(
      externalDependency, [&](const Job *job) {
        foundJobs.push_back(job);
        for (const Job *marked : findJobsToRecompileWhenWholeJobChanges(job)) {
          // findJobsToRecompileWhenWholeJobChanges is reflexive
          // Don't return job twice.
          if (marked != job)
            foundJobs.push_back(marked);
        }
      });
  return foundJobs;
}

void ModuleDepGraph::forEachUntracedJobDirectlyDependentOnExternalSwiftDeps(
    StringRef externalSwiftDeps, function_ref<void(const Job *)> fn) {
  // TODO move nameForDep into key
  // These nodes will depend on the *interface* of the external Decl.
  DependencyKey key(NodeKind::externalDepend, DeclAspect::interface, "",
                    externalSwiftDeps.str());
  for (const ModuleDepGraphNode *useNode : usesByDef[key]) {
    if (!useNode->getHasBeenTraced())
      fn(getJob(useNode->getSwiftDepsOfProvides()));
  }
}

//==============================================================================
// MARK: Integrating SourceFileDepGraph into ModuleDepGraph
//==============================================================================

ModuleDepGraph::Changes ModuleDepGraph::integrate(const SourceFileDepGraph &g,
                                                  StringRef swiftDepsOfJob) {
  FrontendStatsTracer tracer(stats, "fine-grained-dependencies-integrate");

  // When done, disappearedNodes contains the nodes which no longer exist.
  auto disappearedNodes = nodeMap[swiftDepsOfJob.str()];
  // When done, changeDependencyKeys contains a list of keys that changed
  // as a result of this integration.
  // Or if the integration failed, None.
  llvm::Optional<std::unordered_set<ModuleDepGraphNode *>> changedNodes =
      std::unordered_set<ModuleDepGraphNode *>();

  g.forEachNode([&](const SourceFileDepGraphNode *integrand) {
    const auto &key = integrand->getKey();
    auto preexistingMatch = findPreexistingMatch(swiftDepsOfJob, integrand);
    if (preexistingMatch.has_value() &&
        preexistingMatch.value().first == LocationOfPreexistingNode::here)
      disappearedNodes.erase(key); // Node was and still is. Do not erase it.

    llvm::Optional<NullablePtr<ModuleDepGraphNode>> newNodeOrChangedNode =
        integrateSourceFileDepGraphNode(g, integrand, preexistingMatch,
                                        swiftDepsOfJob);

    if (!newNodeOrChangedNode)
      changedNodes = llvm::None;
    else if (!changedNodes)
      ;
    else if (auto *n = newNodeOrChangedNode.value().getPtrOrNull())
      changedNodes.value().insert(n);
  });
  if (!changedNodes)
    return llvm::None;

  for (auto &p : disappearedNodes) {
    changedNodes.value().insert(p.second);
    eraseNodeFromJob(p.second);
  }

  // Make sure the changes can be retraced:
  for (auto *n : changedNodes.value())
    n->clearHasBeenTraced();

  return changedNodes.value();
}

ModuleDepGraph::PreexistingNodeIfAny ModuleDepGraph::findPreexistingMatch(
    StringRef swiftDepsOfCompilationToBeIntegrated,
    const SourceFileDepGraphNode *integrand) const {
  const auto *matches = nodeMap.find(integrand->getKey()).getPtrOrNull();
  if (!matches)
    return llvm::None;
  const auto &expatsIter = matches->find("");
  if (expatsIter != matches->end()) {
    assert(matches->size() == 1 &&
           "If an expat exists, then must not be any matches in other files");
    return std::make_pair(LocationOfPreexistingNode::nowhere,
                          expatsIter->second);
  }
  if (integrand->getIsProvides()) {
    const auto &preexistingNodeInPlaceIter =
        matches->find(swiftDepsOfCompilationToBeIntegrated.str());
    if (preexistingNodeInPlaceIter != matches->end())
      return std::make_pair(LocationOfPreexistingNode::here,
                            preexistingNodeInPlaceIter->second);
  }
  if (!matches->empty())
    return std::make_pair(LocationOfPreexistingNode::elsewhere,
                          matches->begin()->second);
  return llvm::None;
}

llvm::Optional<NullablePtr<ModuleDepGraphNode>>
ModuleDepGraph::integrateSourceFileDepGraphNode(
    const SourceFileDepGraph &g, const SourceFileDepGraphNode *integrand,
    const PreexistingNodeIfAny preexistingMatch,
    const StringRef swiftDepsOfJob) {
  if (!integrand->getIsProvides())
    return NullablePtr<ModuleDepGraphNode>(); // depends are captured by
                                              // recordWhatUseDependsUpon below

  auto changedAndIntegrationResultNode =
      integrateSourceFileDeclNode(integrand, swiftDepsOfJob, preexistingMatch);

  ModuleDepGraphNode *const integratedNode =
      changedAndIntegrationResultNode.second;
  const bool hasNewExternalDependency =
      recordWhatUseDependsUpon(g, integrand, integratedNode);

  NullablePtr<ModuleDepGraphNode> changedNode =
      changedAndIntegrationResultNode.first || hasNewExternalDependency
          ? integratedNode
          : nullptr;
  return changedNode;
}

std::pair<bool, ModuleDepGraphNode *>
ModuleDepGraph::integrateSourceFileDeclNode(
    const SourceFileDepGraphNode *integrand, StringRef swiftDepsOfJob,
    const PreexistingNodeIfAny preexistingMatch) {

  if (!preexistingMatch.has_value()) {
    // The driver will be accessing nodes by the swiftDeps of the job,
    // so pass that in.
    auto *newNode =
        integrateByCreatingANewNode(integrand, swiftDepsOfJob.str());
    return std::make_pair(true, newNode); // New node
  }
  const auto where = preexistingMatch.value().first;
  auto *match = preexistingMatch.value().second;
  switch (where) {
  case LocationOfPreexistingNode::here:
    return std::make_pair(match->integrateFingerprintFrom(integrand), match);

  case LocationOfPreexistingNode::nowhere:
    // Some other file depended on this, but didn't know where it was.
    moveNodeToDifferentFile(match, swiftDepsOfJob.str());
    match->integrateFingerprintFrom(integrand);
    return std::make_pair(true, match); // New Decl, assume changed

  case LocationOfPreexistingNode::elsewhere:
    auto *newNode =
        integrateByCreatingANewNode(integrand, swiftDepsOfJob.str());
    return std::make_pair(true, newNode); // New node;
  }
  llvm_unreachable("impossible");
}

ModuleDepGraphNode *ModuleDepGraph::integrateByCreatingANewNode(
    const SourceFileDepGraphNode *integrand,
    const llvm::Optional<std::string> swiftDepsForNewNode) {
  assert(integrand->getIsProvides() &&
         "Dependencies are arcs in the module graph");
  const auto &key = integrand->getKey();
  ModuleDepGraphNode *newNode = new ModuleDepGraphNode(
      key, integrand->getFingerprint(), swiftDepsForNewNode);
  addToMap(newNode);
  return newNode;
}

bool ModuleDepGraph::recordWhatUseDependsUpon(
    const SourceFileDepGraph &g,
    const SourceFileDepGraphNode *sourceFileUseNode,
    ModuleDepGraphNode *moduleUseNode) {
  bool useHasNewExternalDependency = false;
  g.forEachDefDependedUponBy(
      sourceFileUseNode, [&](const SourceFileDepGraphNode *def) {
        const bool isNewUse =
            usesByDef[def->getKey()].insert(moduleUseNode).second;
        if (isNewUse) {
          StringRef externalSwiftDeps = def->getKey().getName();
          if (def->getKey().getKind() == NodeKind::externalDepend) {
            externalDependencies.insert(externalSwiftDeps.str());
            useHasNewExternalDependency = true;
          }
        }
      });
  return useHasNewExternalDependency;
}

void ModuleDepGraph::eraseNodeFromGraphAndFreeIt(ModuleDepGraphNode *n) {
  eraseNodeFromJob(n);
  eraseNodeFromCurrentPathIfTracing(n);
  eraseNodeFromDependencyPathToJobs(n);

  delete n;
}

void ModuleDepGraph::eraseNodeFromJob(ModuleDepGraphNode *n) {
  eraseNodeFromMap(n);
  eraseUsesOfNode(n);
}

//==============================================================================
// MARK: ModuleDepGraph access
//==============================================================================
void ModuleDepGraph::forEachUseOf(
    const ModuleDepGraphNode *def,
    function_ref<void(ModuleDepGraphNode *)> fn) const {
  auto iter = usesByDef.find(def->getKey());
  if (iter == usesByDef.end())
    return;
  for (ModuleDepGraphNode *useNode : iter->second)
    fn(useNode);
  // Add in implicit interface->implementation dependency
  forCorrespondingImplementationOfProvidedInterface(def, fn);
}

void ModuleDepGraph::forCorrespondingImplementationOfProvidedInterface(
    const ModuleDepGraphNode *interfaceNode,
    function_ref<void(ModuleDepGraphNode *)> fn) const {
  if (!interfaceNode->getKey().isInterface() || !interfaceNode->getIsProvides())
    return;
  const auto swiftDeps = interfaceNode->getSwiftDeps().value();
  const auto &interfaceKey = interfaceNode->getKey();
  const DependencyKey implementationKey(
      interfaceKey.getKind(), DeclAspect::implementation,
      interfaceKey.getContext().str(), interfaceKey.getName().str());
  if (const auto implementationNode =
          nodeMap.find(swiftDeps, implementationKey))
    fn(implementationNode.value());
}

void ModuleDepGraph::forEachNode(
    function_ref<void(ModuleDepGraphNode *)> fn) const {
  nodeMap.forEachEntry([&](const std::string &, const DependencyKey &,
                           ModuleDepGraphNode *n) { fn(n); });
}

void ModuleDepGraph::forEachMatchingNode(
    const DependencyKey &key,
    function_ref<void(ModuleDepGraphNode *)> fn) const {
  nodeMap.forEachValueMatching(
      key, [&](const std::string &, ModuleDepGraphNode *n) { fn(n); });
}

void ModuleDepGraph::forEachArc(
    function_ref<void(const ModuleDepGraphNode *, const ModuleDepGraphNode *)>
        fn) const {
  forEachNode([&](const ModuleDepGraphNode *defNode) {
    forEachUseOf(defNode, [&](const ModuleDepGraphNode *const useNode) {
      fn(defNode, useNode);
    });
  });
}

void ModuleDepGraph::forEachNodeInJob(
    StringRef swiftDeps, function_ref<void(ModuleDepGraphNode *)> fn) const {
  if (const auto *nodesByKeys = nodeMap.find(swiftDeps.str()).getPtrOrNull()) {
    for (const auto &keyAndNode : *nodesByKeys)
      fn(keyAndNode.second);
  }
}

//==============================================================================
// MARK: ModuleDepGraph traversal
//==============================================================================

void ModuleDepGraph::findPreviouslyUntracedDependents(
    std::vector<ModuleDepGraphNode *> &foundDependents,
    ModuleDepGraphNode *definition) {

  if (definition->getHasBeenTraced())
    return;
  definition->setHasBeenTraced();

  foundDependents.push_back(definition);

  // If this use also provides something, follow it
  if (!definition->getIsProvides())
    return; // No need to look for uses; provides nothing

  size_t pathLengthAfterArrival = traceArrival(definition);

  forEachUseOf(definition, [&](ModuleDepGraphNode *u) {
    // If this use also provides something, follow it
    findPreviouslyUntracedDependents(foundDependents, u);
  });

  traceDeparture(pathLengthAfterArrival);
}

size_t ModuleDepGraph::traceArrival(const ModuleDepGraphNode *visitedNode) {
  if (!currentPathIfTracing.has_value())
    return 0;
  auto &currentPath = currentPathIfTracing.value();
  currentPath.push_back(visitedNode);
  const auto visitedSwiftDepsIfAny = visitedNode->getSwiftDeps();
  recordDependencyPathToJob(currentPath, getJob(visitedSwiftDepsIfAny));
  return currentPath.size();
}

void ModuleDepGraph::recordDependencyPathToJob(
    const std::vector<const ModuleDepGraphNode *> &pathToJob,
    const driver::Job *dependentJob) {
  dependencyPathsToJobs.insert(std::make_pair(dependentJob, pathToJob));
}

void ModuleDepGraph::traceDeparture(size_t pathLengthAfterArrival) {
  if (!currentPathIfTracing)
    return;
  auto &currentPath = currentPathIfTracing.value();
  assert(pathLengthAfterArrival == currentPath.size() &&
         "Path must be maintained throughout recursive visits.");
  currentPath.pop_back();
}

// =============================================================================
// MARK: Emitting Dot file for ModuleDepGraph
// =============================================================================

void ModuleDepGraph::emitDotFileForJob(DiagnosticEngine &diags,
                                       const Job *job) {
  emitDotFile(diags, getSwiftDeps(job));
}

void ModuleDepGraph::emitDotFile(DiagnosticEngine &diags,
                                 StringRef baseName) {
  unsigned seqNo = dotFileSequenceNumber[baseName.str()]++;
  std::string fullName =
      baseName.str() + "-post-integration." + std::to_string(seqNo) + ".dot";
  withOutputPath(diags, *backend, fullName, [&](llvm::raw_ostream &out) {
    emitDotFile(out);
    return false;
  });
}

void ModuleDepGraph::emitDotFile(llvm::raw_ostream &out) {
  FrontendStatsTracer tracer(stats, "fine-grained-dependencies-emitDotFile");
  DotFileEmitter<ModuleDepGraph>(out, *this, true, false).emit();
}

//==============================================================================
// MARK: ModuleDepGraph debugging
//==============================================================================

void ModuleDepGraphNode::dump(llvm::raw_ostream &out) const {
  DepGraphNode::dump(out);
  if (getIsProvides())
    out << " swiftDeps: <" << getSwiftDepsOfProvides() << ">\n";
  else
    out << " no swiftDeps\n";
}

void ModuleDepGraphNode::dump() const {
  DepGraphNode::dump();
  if (getIsProvides())
    llvm::errs() << " swiftDeps: <" << getSwiftDepsOfProvides() << ">\n";
  else
    llvm::errs() << " no swiftDeps\n";
}

bool ModuleDepGraph::verify() const {
  FrontendStatsTracer tracer(stats, "fine-grained-dependencies-verify");
  verifyNodeMapEntries();
  verifyCanFindEachJob();
  verifyEachJobInGraphIsTracked();

  return true;
}

void ModuleDepGraph::verifyNodeMapEntries() const {
  FrontendStatsTracer tracer(stats,
                             "fine-grained-dependencies-verifyNodeMapEntries");
  // TODO: disable when not debugging
  std::array<
      std::unordered_map<DependencyKey,
                         std::unordered_map<std::string, ModuleDepGraphNode *>>,
      2>
      nodesSeenInNodeMap;
  nodeMap.verify([&](const std::string &swiftDepsString,
                     const DependencyKey &key, ModuleDepGraphNode *n,
                     unsigned submapIndex) {
    verifyNodeMapEntry(nodesSeenInNodeMap, swiftDepsString, key, n,
                       submapIndex);
  });
}

void ModuleDepGraph::verifyNodeMapEntry(
    std::array<std::unordered_map<
                   DependencyKey,
                   std::unordered_map<std::string, ModuleDepGraphNode *>>,
               2> &nodesSeenInNodeMap,
    const std::string &swiftDepsString, const DependencyKey &key,
    ModuleDepGraphNode *n, const unsigned submapIndex) const {
  verifyNodeIsUniqueWithinSubgraph(nodesSeenInNodeMap, swiftDepsString, key, n,
                                   submapIndex);
  verifyNodeIsInRightEntryInNodeMap(swiftDepsString, key, n);
  key.verify();
  verifyExternalDependencyUniqueness(key);
}

void ModuleDepGraph::verifyNodeIsUniqueWithinSubgraph(
    std::array<std::unordered_map<
                   DependencyKey,
                   std::unordered_map<std::string, ModuleDepGraphNode *>>,
               2> &nodesSeenInNodeMap,
    const std::string &swiftDepsString, const DependencyKey &key,
    ModuleDepGraphNode *const n, const unsigned submapIndex) const {
  assert(submapIndex < nodesSeenInNodeMap.size() &&
         "submapIndex is out of bounds.");
  auto iterInserted = nodesSeenInNodeMap[submapIndex][n->getKey()].insert(
      std::make_pair(n->getSwiftDepsForMapKey(), n));
  if (!iterInserted.second) {
    llvm_unreachable("duplicate driver keys");
  }
}

void ModuleDepGraph::verifyNodeIsInRightEntryInNodeMap(
    const std::string &swiftDepsString, const DependencyKey &key,
    const ModuleDepGraphNode *const n) const {
  const DependencyKey &nodeKey = n->getKey();
  const llvm::Optional<std::string> swiftDeps =
      swiftDepsString.empty() ? llvm::None
                              : llvm::Optional<std::string>(swiftDepsString);
  (void)nodeKey;
  (void)swiftDeps;
  assert(n->getSwiftDeps() == swiftDeps ||
         mapCorruption("Node misplaced for swiftDeps"));
  assert(nodeKey == key || mapCorruption("Node misplaced for key"));
}

void ModuleDepGraph::verifyExternalDependencyUniqueness(
    const DependencyKey &key) const {
  assert((key.getKind() != NodeKind::externalDepend ||
          externalDependencies.count(key.getName().str()) == 1) &&
         "Ensure each external dependency is tracked exactly once");
}

void ModuleDepGraph::verifyCanFindEachJob() const {
  FrontendStatsTracer tracer(stats,
                             "fine-grained-dependencies-verifyCanFindEachJob");
  for (const auto &p : jobsBySwiftDeps) {
    getJob(p.first);
  }
}

void ModuleDepGraph::verifyEachJobInGraphIsTracked() const {
  FrontendStatsTracer tracer(
      stats, "fine-grained-dependencies-verifyEachJobIsTracked");
  nodeMap.forEachKey1(
      [&](const std::string &swiftDeps, const typename NodeMap::Key2Map &) {
        ensureJobIsTracked(swiftDeps);
      });
}

/// Dump the path(s) that led to \p node.
/// TODO: break up
void ModuleDepGraph::printPath(raw_ostream &out,
                               const driver::Job *jobToBeBuilt) const {
  assert(currentPathIfTracing.has_value() &&
         "Cannot print paths of paths weren't tracked.");

  for (auto paths = dependencyPathsToJobs.find(jobToBeBuilt);
       paths != dependencyPathsToJobs.end() && paths->first == jobToBeBuilt;
       ++paths) {
    const auto &path = paths->second;
    bool first = true;
    out << "\t";
    for (const ModuleDepGraphNode *n : path) {
      if (first)
        first = false;
      else
        out << " -> ";

      const StringRef providerName = getProvidingFilename(n->getSwiftDeps());
      printOneNodeOfPath(out, n->getKey(), providerName);
    }
    out << "\n";
  }
}

StringRef ModuleDepGraph::getProvidingFilename(
    const llvm::Optional<std::string> &swiftDeps) const {
  if (!swiftDeps)
    return "<unknown";
  auto ext = llvm::sys::path::extension(*swiftDeps);
  if (file_types::lookupTypeForExtension(ext) ==
      file_types::TY_SwiftModuleFile) {
    return *swiftDeps;
  }
  const StringRef inputName =
      llvm::sys::path::filename(getJob(swiftDeps)->getFirstSwiftPrimaryInput());
  // FineGrainedDependencyGraphTests work with simulated jobs with empty
  // input names.
  return !inputName.empty() ? inputName : StringRef(swiftDeps.value());
}

void ModuleDepGraph::printOneNodeOfPath(raw_ostream &out,
                                        const DependencyKey &key,
                                        const StringRef filename) {
  switch (key.getKind()) {
  case NodeKind::topLevel:
    out << key.aspectName() << " of top-level name '" << key.humanReadableName()
        << "' in " << filename;
    break;
  case NodeKind::nominal:
    out << key.aspectName() << " of type '" << key.humanReadableName()
        << "' in " << filename;
    break;
  case NodeKind::potentialMember:
    out << key.aspectName() << " of non-private members '"
        << key.humanReadableName() << "' in " << filename;
    break;
  case NodeKind::member:
    out << key.aspectName() << " of member '" << key.humanReadableName()
        << "' in " << filename;
    break;
  case NodeKind::dynamicLookup:
    out << key.aspectName() << " of AnyObject member '"
        << key.humanReadableName() << "' in " << filename;
    break;
  case NodeKind::externalDepend:
    out << filename << " depends on " << key.aspectName() << " of module '"
        << key.humanReadableName() << "'";
    break;
  case NodeKind::sourceFileProvide:
    out << key.aspectName() << " of source file " << key.humanReadableName();
    break;
  default:
    llvm_unreachable("unknown NodeKind");
  }
}
