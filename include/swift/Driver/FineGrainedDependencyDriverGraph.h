//===---- FineGrainedDependencyModuleDepGraph.h ------------------*- C++-*-===//
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

#ifndef SWIFT_DRIVER_FINE_GRAINED_DEPENDENCY_DRIVER_GRAPH_H
#define SWIFT_DRIVER_FINE_GRAINED_DEPENDENCY_DRIVER_GRAPH_H

#include "swift/AST/FineGrainedDependencies.h"
#include "swift/Basic/Debug.h"
#include "swift/Basic/Fingerprint.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/OptionSet.h"
#include "swift/Driver/Job.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/PointerLikeTypeTraits.h"
#include "llvm/Support/VirtualOutputBackend.h"
#include "llvm/Support/VirtualOutputBackends.h"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Declarations for the portion fine-grained dependency system used by the
// driver.

namespace swift {

class UnifiedStatsReporter;

namespace fine_grained_dependencies {

//==============================================================================
// MARK: ModuleDepGraphNode
//==============================================================================

/// A node in the DriverDependencyGraph
/// Keep separate type from Node for type-checking.
class ModuleDepGraphNode : public DepGraphNode {

  /// The swiftDeps file that holds this entity iff this is a provides node.
  /// If more than one source file has the same DependencyKey, then there
  /// will be one node for each in the driver, distinguished by this field.
  llvm::Optional<std::string> swiftDeps;

  /// When finding transitive dependents, this node has been traversed.
  bool hasBeenTracedAsADependent = false;

public:
  ModuleDepGraphNode(const DependencyKey &key,
                     llvm::Optional<Fingerprint> fingerprint,
                     llvm::Optional<std::string> swiftDeps)
      : DepGraphNode(key, fingerprint), swiftDeps(swiftDeps) {}

  bool getHasBeenTraced() const { return hasBeenTracedAsADependent; }
  void setHasBeenTraced() { hasBeenTracedAsADependent = true; }
  void clearHasBeenTraced() { hasBeenTracedAsADependent = false; }

  /// Integrate \p integrand's fingerprint into \p dn.
  /// \returns true if there was a change requiring recompilation.
  bool integrateFingerprintFrom(const SourceFileDepGraphNode *integrand) {
    if (getFingerprint() == integrand->getFingerprint())
      return false;
    setFingerprint(integrand->getFingerprint());
    return true;
  }

  bool operator==(const ModuleDepGraphNode &other) const {
    return static_cast<DepGraphNode>(*this) ==
               static_cast<DepGraphNode>(other) &&
           getSwiftDeps() == other.getSwiftDeps();
  }

  const llvm::Optional<std::string> &getSwiftDeps() const { return swiftDeps; }

  std::string getSwiftDepsOrEmpty() const {
    return getSwiftDeps().value_or(std::string());
  }

  std::string getSwiftDepsForMapKey() const {
    // Use the empty string for nodes whose source file is unknown,
    // i.e. depends. (Known depends are represented by arcs, not nodes.)
    return getSwiftDepsOrEmpty();
  }

  const std::string &getSwiftDepsOfProvides() const {
    return getSwiftDeps().value();
  }

  /// Nodes can move from file to file when the driver reads the result of a
  /// compilation.
  void setSwiftDeps(llvm::Optional<std::string> s) { swiftDeps = s; }

  bool getIsProvides() const { return getSwiftDeps().has_value(); }

  /// Return true if this node describes a definition for which the job is known
  bool isDefinedInAKnownFile() const { return getIsProvides(); }

  bool doesNodeProvideAnInterface() const {
    return getKey().isInterface() && getIsProvides();
  }

  bool assertImplementationMustBeInAFile() const {
    assert((isDefinedInAKnownFile() || !getKey().isImplementation()) &&
           "Implementations must be in some file.");
    return true;
  }

  std::string humanReadableName() const {
    StringRef where = !getIsProvides()
                          ? ""
                          : llvm::sys::path::filename(getSwiftDepsOfProvides());
    return DepGraphNode::humanReadableName(where);
  }

  void dump(raw_ostream &) const;

  SWIFT_DEBUG_DUMP;
};

//==============================================================================
// MARK: ModuleDepGraph
//==============================================================================

/// See \ref Node in FineGrainedDependencies.h
class ModuleDepGraph {

  /// Find nodes, first by the swiftDeps file, then by key.
  /// Supports searching specific files for a node matching a key.
  /// Such a search is useful when integrating nodes from a given source file to
  /// see which nodes were there before integration and so might have
  /// disappeared.
  ///
  /// Some nodes are in no file, for instance a dependency on a Decl in a source
  /// file whose swiftdeps has not been read yet. For these, the filename is the
  /// empty string.
  ///
  /// Don't add to this collection directly; use \ref addToMap
  /// instead because it enforces the correspondence with the swiftFileDeps
  /// field of the node.
  /// TODO: Fix above comment
  ///
  /// Sadly, cannot use an optional string for a key.
  using NodeMap =
      BiIndexedTwoStageMap<std::string, DependencyKey, ModuleDepGraphNode *>;
  NodeMap nodeMap;

  /// Since dependency keys use baseNames, they are coarser than individual
  /// decls. So two decls might map to the same key. Given a use, which is
  /// denoted by a node, the code needs to find the files to recompile. So, the
  /// key indexes into the nodeMap, and that yields a submap of nodes keyed by
  /// file. The set of keys in the submap are the files that must be recompiled
  /// for the use.
  /// (In a given file, only one node exists with a given key, but in the future
  /// that would need to change if/when we can recompile a smaller unit than a
  /// source file.)

  /// Tracks def-use relationships by DependencyKey.
  std::unordered_map<DependencyKey, std::unordered_set<ModuleDepGraphNode *>>
      usesByDef;

  // Supports requests from the driver to getExternalDependencies.
  std::unordered_set<std::string> externalDependencies;

  /// Keyed by swiftdeps filename, so we can get back to Jobs.
  std::unordered_map<std::string, const driver::Job *> jobsBySwiftDeps;

  /// For debugging, a dot file can be emitted. This file can be read into
  /// various graph-drawing programs.
  /// The driver emits this file into the same directory as the swiftdeps
  /// files it reads, so when reading a file compute the base path here.
  /// Initialize to empty in case no swiftdeps file has been read.
  SmallString<128> driverDotFileBasePath = StringRef("");

  /// For debugging, the driver can write out a dot file, for instance when a
  /// Frontend swiftdeps is read and integrated. In order to keep subsequent
  /// files for the same name distinct, keep a sequence number for each name.
  std::unordered_map<std::string, unsigned> dotFileSequenceNumber;

public:
  bool verifyFineGrainedDependencyGraphAfterEveryImport;
  bool emitFineGrainedDependencyDotFileAfterEveryImport;

private:
  /// If tracing dependencies, holds a vector used to hold the current path
  /// def - use/def - use/def - ...
  llvm::Optional<std::vector<const ModuleDepGraphNode *>> currentPathIfTracing;

  /// If tracing dependencies, holds the sequence of defs used to get to the job
  /// that is the key
  std::unordered_multimap<const driver::Job *,
                          std::vector<const ModuleDepGraphNode *>>
      dependencyPathsToJobs;

  /// VirtualOutputBackend for emitting graphs.
  llvm::IntrusiveRefCntPtr<llvm::vfs::OutputBackend> backend;

  /// For helping with performance tuning, may be null:
  UnifiedStatsReporter *stats;

  //==============================================================================
  // MARK: ModuleDepGraph - mutating dependencies
  //==============================================================================

  /// Encapsulate the invariant between where the node resides in
  /// nodesBySwiftDepsFile and the swiftDeps node instance variable here.
  void addToMap(ModuleDepGraphNode *n) {
    nodeMap.insert(n->getSwiftDepsForMapKey(), n->getKey(), n);
  }

  /// When integrating a SourceFileDepGraph, there might be a node representing
  /// a Decl that had previously been read as an expat, that is a node
  /// representing a Decl in no known file (to that point). (Recall the the
  /// Frontend processes name lookups as dependencies, but does not record in
  /// which file the name was found.) In such a case, it is necessary to move
  /// the node to the proper collection.
  void moveNodeToDifferentFile(ModuleDepGraphNode *n,
                               llvm::Optional<std::string> newFile) {
    eraseNodeFromMap(n);
    n->setSwiftDeps(newFile);
    addToMap(n);
  }

  /// Remove node from nodeMap, check invariants.
  ModuleDepGraphNode *eraseNodeFromMap(ModuleDepGraphNode *nodeToErase) {
    ModuleDepGraphNode *nodeActuallyErased = nodeMap.findAndErase(
        nodeToErase->getSwiftDepsForMapKey(), nodeToErase->getKey());
    (void)nodeActuallyErased;
    assert(
        nodeToErase == nodeActuallyErased ||
        mapCorruption("Node found from key must be same as node holding key."));
    return nodeToErase;
  }

  void eraseUsesOfNode(ModuleDepGraphNode *nodeToErase) {
    for (auto &defAndUses : usesByDef)
      defAndUses.second.erase(nodeToErase);
  }

  void eraseNodeFromCurrentPathIfTracing(ModuleDepGraphNode *nodeToErase) {
    if (currentPathIfTracing)
      eraseNodeFromVector(currentPathIfTracing.value(), nodeToErase);
  }

  void eraseNodeFromDependencyPathToJobs(ModuleDepGraphNode *nodeToErase) {
    for (auto &jobAndPath : dependencyPathsToJobs)
      eraseNodeFromVector(jobAndPath.second, nodeToErase);
  }

  static void eraseNodeFromVector(std::vector<const ModuleDepGraphNode *> &v,
                                  const ModuleDepGraphNode *n) {
    const auto where = std::find(v.begin(), v.end(), n);
    if (where != v.end())
      v.erase(where);
  }

  void eraseNodeFromGraphAndFreeIt(ModuleDepGraphNode *);

  /// If the programmer removes a Decl from a source file, the corresponding
  /// ModuleDepGraphNode needs to be removed.
  void eraseNodeFromJob(ModuleDepGraphNode *);

  //============================================================================
  // MARK: ModuleDepGraph - utilities for Job and swiftdeps
  //============================================================================
private:
  static StringRef getSwiftDeps(const driver::Job *cmd) {
    return cmd->getOutput().getAdditionalOutputForType(
        file_types::TY_SwiftDeps);
  }

  const driver::Job *getJob(llvm::Optional<std::string> swiftDeps) const {
    assert(swiftDeps.has_value() && "Don't call me for expats.");
    auto iter = jobsBySwiftDeps.find(swiftDeps.value());
    assert(iter != jobsBySwiftDeps.end() && "All jobs should be tracked.");
    return iter->second;
  }

  //============================================================================
  // MARK: ModuleDepGraph - creation
  //============================================================================

public:
  /// For templates such as DotFileEmitter.
  using NodeType = ModuleDepGraphNode;

  /// \p stats may be null
  ModuleDepGraph(const bool verifyFineGrainedDependencyGraphAfterEveryImport,
                 const bool emitFineGrainedDependencyDotFileAfterEveryImport,
                 const bool shouldTraceDependencies,
                 UnifiedStatsReporter *stats)
      : verifyFineGrainedDependencyGraphAfterEveryImport(
            verifyFineGrainedDependencyGraphAfterEveryImport),
        emitFineGrainedDependencyDotFileAfterEveryImport(
            emitFineGrainedDependencyDotFileAfterEveryImport),
        currentPathIfTracing(
            shouldTraceDependencies
                ? llvm::Optional<std::vector<const ModuleDepGraphNode *>>(
                      std::vector<const ModuleDepGraphNode *>())
                : llvm::None),
        stats(stats) {
    assert(verify() && "ModuleDepGraph should be fine when created");

    // Create a OnDiskOutputBackend for emitting graphs. Currently, this is
    // only used by driver so the backend is not shared with a CompilerInstance.
    backend = llvm::makeIntrusiveRefCnt<llvm::vfs::OnDiskOutputBackend>();
  }

  /// For unit tests.
  ModuleDepGraph(const bool EmitDotFilesForDebugging = false)
      : ModuleDepGraph(
            true, /*emitFineGrainedDependencyDotFileAfterEveryImport=*/
            EmitDotFilesForDebugging, false, nullptr) {}

  //============================================================================
  // MARK: ModuleDepGraph - updating from a switdeps file
  //============================================================================
public:
  using Changes = llvm::Optional<std::unordered_set<ModuleDepGraphNode *>>;

  /// Unlike the standard \c CoarseGrainedDependencyGraph, returns \c
  /// CoarseGrainedDependencyGraphImpl::LoadResult::AffectsDownstream when
  /// loading a new file, i.e. when determining the initial set. Caller
  /// compensates.
  Changes loadFromPath(const driver::Job *, StringRef, DiagnosticEngine &);

  Changes loadFromSourceFileDepGraph(const driver::Job *cmd,
                                     const SourceFileDepGraph &,
                                     DiagnosticEngine &);

  Changes loadFromSwiftModuleBuffer(const driver::Job *, llvm::MemoryBuffer &,
                                    DiagnosticEngine &);

private:
  /// Read a SourceFileDepGraph belonging to \p job from \p buffer
  /// and integrate it into the ModuleDepGraph.
  /// Used both the first time, and to reload the SourceFileDepGraph.
  /// If any changes were observed, indicate same in the return vale.
  Changes loadFromBuffer(const driver::Job *, llvm::MemoryBuffer &,
                         DiagnosticEngine &);

  /// Integrate a SourceFileDepGraph into the receiver.
  /// Integration happens when the driver needs to read SourceFileDepGraph.
  Changes
  integrate(const SourceFileDepGraph &, StringRef swiftDepsOfJob);

  enum class LocationOfPreexistingNode { nowhere, here, elsewhere };

  typedef llvm::Optional<
      std::pair<LocationOfPreexistingNode, ModuleDepGraphNode *>>
      PreexistingNodeIfAny;

  /// Find the preexisting node here that best matches the integrand.
  PreexistingNodeIfAny
  findPreexistingMatch(StringRef swiftDepsOfCompilationToBeIntegrated,
                       const SourceFileDepGraphNode *integrand) const;

  /// Integrate the \p integrand into the receiver.
  /// If an illegal value was found, return \c None, otherwise
  /// return the changed node if any..
  llvm::Optional<NullablePtr<ModuleDepGraphNode>>
  integrateSourceFileDepGraphNode(const SourceFileDepGraph &g,
                                  const SourceFileDepGraphNode *integrand,
                                  const PreexistingNodeIfAny preexistingMatch,
                                  StringRef swiftDepsOfJob);

  /// Integrate the \p integrand, a node that represents a Decl in the swiftDeps
  /// file being integrated. \p preexistingNodeInPlace holds the node
  /// representing the same Decl that already exists, if there is one. \p
  /// preexistingExpat holds a node with the same key that already exists, but was
  /// not known to reside in any swiftDeps file. Return a bool indicating if
  /// this node represents a change that must be propagated, and the integrated
  /// ModuleDepGraphNode.
  std::pair<bool, ModuleDepGraphNode *>
  integrateSourceFileDeclNode(const SourceFileDepGraphNode *integrand,
                              StringRef swiftDepsOfJob,
                              const PreexistingNodeIfAny preexistingMatch);

  /// Create a brand-new ModuleDepGraphNode to integrate \p integrand.
  ModuleDepGraphNode *
  integrateByCreatingANewNode(const SourceFileDepGraphNode *integrand,
                              llvm::Optional<std::string> swiftDepsForNewNode);

  /// After importing a provides node from the frontend, record its
  /// dependencies.
  /// Return true if moduleUseNode picks up a new external-dependency
  ///
  /// \param g The source file graph being integrated into the module graph
  /// \param sourceFileUseNode  The source file node just integrated, which may
  /// also be a use (i.e. a "depends", a declaration used by something else)
  /// \param moduleUseNode The module file node corresponding to the \c
  /// sourceFileUseNode
  bool recordWhatUseDependsUpon(const SourceFileDepGraph &g,
                                const SourceFileDepGraphNode *sourceFileUseNode,
                                ModuleDepGraphNode *moduleUseNode);

  //============================================================================
  // MARK: ModuleDepGraph - dot file support
  //============================================================================
public:
  /// For the dot file.
  std::string getGraphID() const { return "driver"; }

  /// Don't want to do this after every integration--too slow--
  /// So export this hook to the driver.
  bool emitDotFileAndVerify(DiagnosticEngine &) const;

  /// Use the known swiftDeps to find a directory for
  /// the job-independent dot file.
  std::string computePathForDotFile() const;

  /// For debugging and visualization, write out the graph to a dot file.
  /// \p diags may be null if no diagnostics are needed.
  void emitDotFileForJob(DiagnosticEngine &, const driver::Job *);
  void emitDotFile(DiagnosticEngine &, StringRef baseName);
  void emitDotFile() { emitDotFile(llvm::errs()); }
  void emitDotFile(llvm::raw_ostream &);

  //============================================================================
  // MARK: ModuleDepGraph - traversal
  //============================================================================
public:
  void forCorrespondingImplementationOfProvidedInterface(
      const ModuleDepGraphNode *,
      function_ref<void(ModuleDepGraphNode *)>) const;

  void forEachUseOf(const ModuleDepGraphNode *def,
                    function_ref<void(ModuleDepGraphNode *use)>) const;

  void forEachNode(function_ref<void(ModuleDepGraphNode *)>) const;

  void forEachArc(function_ref<void(const ModuleDepGraphNode *def,
                                    const ModuleDepGraphNode *use)>) const;

  /// Call \p fn for each node whose key matches \p key.
  void
  forEachMatchingNode(const DependencyKey &key,
                      function_ref<void(ModuleDepGraphNode *)>) const;

  void forEachNodeInJob(StringRef swiftDeps,
                        function_ref<void(ModuleDepGraphNode *)>) const;

  /// Given a definition node, transitively find all previous untraced
  /// dependents and add them to the array. Also returns definition if that is
  /// untraced.
  void findPreviouslyUntracedDependents(
      std::vector<ModuleDepGraphNode *> &foundDependents,
      ModuleDepGraphNode *definition);

  /// Given a set of nodes, return the set of swiftDeps for the jobs those
  /// nodes are in.
  std::vector<std::string>
  computeSwiftDepsFromNodes(ArrayRef<const ModuleDepGraphNode *> nodes) const;

  /// Record a visit to this node for later dependency printing
  size_t traceArrival(const ModuleDepGraphNode *visitedNode);
  /// Record end of visit to this node.
  void traceDeparture(size_t pathLengthAfterArrival);

  /// For printing why a Job was compiled, record how it was found.
  void recordDependencyPathToJob(
      const std::vector<const ModuleDepGraphNode *> &pathToJob,
      const driver::Job *dependentJob);

  /// Dump the path that led to \p node.
  void printPath(raw_ostream &out, const driver::Job *node) const;

  /// Get a printable filename, given a node's swiftDeps.
  StringRef
  getProvidingFilename(const llvm::Optional<std::string> &swiftDeps) const;

  /// Print one node on the dependency path.
  static void printOneNodeOfPath(raw_ostream &out, const DependencyKey &key,
                                 const StringRef filename);

  bool isCurrentPathForTracingEmpty() const {
    return !currentPathIfTracing.has_value() || currentPathIfTracing->empty();
  }

  //============================================================================
  // MARK: ModuleDepGraph - job-level queries and operations
  //============================================================================
public:
  // This section contains the interface to the status quo code in the driver.

  bool haveAnyNodesBeenTraversedIn(const driver::Job *) const;

  /// Find all jobs (possibly including the argument) requiring recompilation
  /// assuming that every entity in \p jobToBeRecompiled has changed.
  std::vector<const driver::Job *>
  findJobsToRecompileWhenWholeJobChanges(const driver::Job *jobToBeRecompiled);

  template <typename Nodes>
  std::vector<const driver::Job *>
  findJobsToRecompileWhenNodesChange(const Nodes &nodes) {
    std::vector<ModuleDepGraphNode *> foundDependents;
    for (ModuleDepGraphNode *n : nodes)
      findPreviouslyUntracedDependents(foundDependents, n);
    return jobsContaining(foundDependents);
  }

private:
  std::vector<const driver::Job *>
  jobsContaining(const ArrayRef<const ModuleDepGraphNode *> uses) const;

public:
  /// Record a new (to this graph) Job.
  void registerJob(const driver::Job *);

  std::vector<const driver::Job *> getAllJobs() const;

  /// Find jobs that were previously not known to need compilation but that
  /// depend on \c externalDependency.
  std::vector<const driver::Job *>
  findExternallyDependentUntracedJobs(StringRef externalDependency);

  //============================================================================
  // MARK: ModuleDepGraph - External dependencies
  //============================================================================

public:
  std::vector<StringRef> getExternalDependencies() const;

  void forEachUntracedJobDirectlyDependentOnExternalSwiftDeps(
      StringRef externalDependency, function_ref<void(const driver::Job *)> fn);
  void forEachUntracedJobDirectlyDependentOnExternalIncrementalSwiftDeps(
      StringRef externalDependency, function_ref<void(const driver::Job *)> fn);

  //============================================================================
  // MARK: ModuleDepGraph - verification
  //============================================================================

private:
  /// Return true or abort
  bool verify() const;

  void verifyNodeMapEntries() const;

  /// Called for each \ref nodeMap entry during verification.
  /// \p nodesSeenInNodeMap ensures that nodes are unique in each submap
  /// \p swiftDepsString is the swiftdeps file name in the map
  /// \p key is the DependencyKey in the map
  /// \p n is the node for that map entry
  void verifyNodeMapEntry(
      std::array<std::unordered_map<
                     DependencyKey,
                     std::unordered_map<std::string, ModuleDepGraphNode *>>,
                 2> &nodesSeenInNodeMap,
      const std::string &swiftDepsString, const DependencyKey &,
      ModuleDepGraphNode *, unsigned submapIndex) const;

  /// See ModuleDepGraph::verifyNodeMapEntry for argument descriptions
  void verifyNodeIsUniqueWithinSubgraph(
      std::array<std::unordered_map<
                     DependencyKey,
                     std::unordered_map<std::string, ModuleDepGraphNode *>>,
                 2> &nodesSeenInNodeMap,
      const std::string &swiftDepsString, const DependencyKey &,
      ModuleDepGraphNode *, unsigned submapIndex) const;

  /// See ModuleDepGraph::verifyNodeMapEntry for argument descriptions
  void verifyNodeIsInRightEntryInNodeMap(const std::string &swiftDepsString,
                                         const DependencyKey &,
                                         const ModuleDepGraphNode *) const;

  void verifyExternalDependencyUniqueness(const DependencyKey &) const;

  void verifyCanFindEachJob() const;
  void verifyEachJobInGraphIsTracked() const;

  static bool mapCorruption(const char *msg) { llvm_unreachable(msg); }


  bool ensureJobIsTracked(const std::string &swiftDeps) const {
    assert(swiftDeps.empty() || getJob(swiftDeps));
    return true;
  }

  static std::vector<const driver::Job *>
  printJobsForDebugging(const std::vector<const driver::Job *> &jobs);
};
} // namespace fine_grained_dependencies
} // namespace swift

#endif // SWIFT_DRIVER_FINE_GRAINED_DEPENDENCY_DRIVER_GRAPH_H
