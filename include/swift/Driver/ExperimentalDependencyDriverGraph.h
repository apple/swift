//===--- ExperimentalDependencyModuleDepGraph.h ------------------*- C++-*-===//
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

#ifndef ExperimentalDependencyGraph_h
#define ExperimentalDependencyGraph_h

#include "swift/AST/ExperimentalDependencies.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/OptionSet.h"
#include "swift/Driver/DependencyGraph.h"
#include "swift/Driver/Job.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/PointerLikeTypeTraits.h"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Declarations for the portion experimental dependency system used by the
// driver.

namespace swift {
namespace experimental_dependencies {

//==============================================================================
// MARK: ModuleDepGraphNode
//==============================================================================

/// A node in the DriverDependencyGraph
/// Keep separate type from Node for type-checking.
class ModuleDepGraphNode : public DepGraphNode {

  /// The swiftDeps file that holds this entity.
  /// If more than one source file has the same DependencyKey, then there
  /// will be one node for each in the driver.
  Optional<std::string> swiftDeps;

public:
  ModuleDepGraphNode(const DependencyKey &key,
                     Optional<std::string> fingerprint,
                     Optional<std::string> swiftDeps)
      : DepGraphNode(key, fingerprint), swiftDeps(swiftDeps) {}

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

  const Optional<std::string> &getSwiftDeps() const { return swiftDeps; }

  bool assertImplementationMustBeInAFile() const {
    assert((getSwiftDeps().hasValue() || !getKey().isImplementation()) &&
           "Implementations must be in some file.");
    return true;
  }

  std::string humanReadableName() const {
    StringRef where =
        !getSwiftDeps().hasValue()
            ? ""
            : llvm::sys::path::filename(getSwiftDeps().getValue());
    return DepGraphNode::humanReadableName(where);
  }

  void dump() const;

  bool assertProvidedEntityMustBeInAFile() const {
    assert((getSwiftDeps().hasValue() || !getKey().isImplementation()) &&
           "Implementations must be in some file.");
    return true;
  }

  /// Nodes can move from file to file when the driver reads the result of a
  /// compilation.
  void setSwiftDeps(Optional<std::string> s) { swiftDeps = s; }

  bool getIsProvides() const { return getSwiftDeps().hasValue(); }
};

/// A placeholder allowing the experimental system to fit into the driver
/// without changing as much code.
class DependencyGraphImpl {
public:
  /// Use the status quo LoadResult for now.
  using LoadResult = typename swift::DependencyGraphImpl::LoadResult;
};

//==============================================================================
// MARK: ModuleDepGraph
//==============================================================================

/// See \ref Node in ExperimentalDependencies.h
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
  /// denoted by a key, the code needs to find the files to recompile. So, the
  /// key indexes into the nodeMap, and that yields a submap of nodes keyed by
  /// file. The set of keys in the submap are the files that must be recompiled
  /// for the use.
  /// (In a given file, only one node exists with a given key, but in the future
  /// that would need to change if/when we can recompile a smaller unit than a
  /// source file.)

  /// Tracks def-use relationships by DependencyKey.
  std::unordered_map<DependencyKey, std::unordered_set<DependencyKey>>
      usesByDef;

  // Supports requests from the driver to getExternalDependencies.
  std::unordered_set<std::string> externalDependencies;

  /// The new version of "Marked."
  /// Record cascading jobs by swiftDepsFilename because that's what
  /// nodes store directly.
  ///
  /// The status quo system uses "cascade" for the following:
  /// Def1 -> def2 -> def3, where arrows are uses, so 3 depends on 2 which
  /// depends on 1. The first use is said to "cascade" if when def1 changes,
  /// def3 is dirtied.
  /// TODO: Move cascadingJobs out of the graph, ultimately.
  /// If marked, any Job that depends on me must be rebuilt after compiling me
  /// if I have changed.

  std::unordered_set<std::string> cascadingJobs;

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

  const bool verifyExperimentalDependencyGraphAfterEveryImport;
  const bool emitExperimentalDependencyDotFileAfterEveryImport;

  /// For helping with performance tuning, may be null:
  UnifiedStatsReporter *const stats;

  /// Encapsulate the invariant between where the node resides in
  /// nodesBySwiftDepsFile and the swiftDeps node instance variable here.
  void addToMap(ModuleDepGraphNode *n) {
    nodeMap.insert(n->getSwiftDeps().getValueOr(std::string()), n->getKey(), n);
  }

  /// When integrating a SourceFileDepGraph, there might be a node representing
  /// a Decl that had previously been read as an expat, that is a node
  /// representing a Decl in no known file (to that point). (Recall the the
  /// Frontend processes name lookups as dependencies, but does not record in
  /// which file the name was found.) In such a case, it is necessary to move
  /// the node to the proper collection.
  void moveNodeToDifferentFile(ModuleDepGraphNode *n,
                               Optional<std::string> newFile) {
    eraseNodeFromMap(n);
    n->setSwiftDeps(newFile);
    addToMap(n);
  }

  /// Remove node from nodeMap, check invariants.
  ModuleDepGraphNode *eraseNodeFromMap(ModuleDepGraphNode *nodeToErase) {
    ModuleDepGraphNode *nodeActuallyErased = nodeMap.findAndErase(
        nodeToErase->getSwiftDeps().getValueOr(std::string()),
        nodeToErase->getKey());
    assert(
        nodeToErase == nodeActuallyErased ||
        mapCorruption("Node found from key must be same as node holding key."));
    return nodeToErase;
  }

  static StringRef getSwiftDeps(const driver::Job *cmd) {
    return cmd->getOutput().getAdditionalOutputForType(
        file_types::TY_SwiftDeps);
  }

  const driver::Job *getJob(Optional<std::string> swiftDeps) const {
    assert(swiftDeps.hasValue() && "Don't call me for expats.");
    auto iter = jobsBySwiftDeps.find(swiftDeps.getValue());
    assert(iter != jobsBySwiftDeps.end() && "All jobs should be tracked.");
    assert(getSwiftDeps(iter->second) == swiftDeps.getValue() &&
           "jobsBySwiftDeps should be inverse of getSwiftDeps.");
    return iter->second;
  }

public:
  /// For templates such as DotFileEmitter.
  using NodeType = ModuleDepGraphNode;

  /// \p stats may be null
  ModuleDepGraph(const bool verifyExperimentalDependencyGraphAfterEveryImport,
                 const bool emitExperimentalDependencyDotFileAfterEveryImport,
                 UnifiedStatsReporter *stats)
      : verifyExperimentalDependencyGraphAfterEveryImport(
            verifyExperimentalDependencyGraphAfterEveryImport),
        emitExperimentalDependencyDotFileAfterEveryImport(
            emitExperimentalDependencyDotFileAfterEveryImport),
        stats(stats) {
    assert(verify() && "ModuleDepGraph should be fine when created");
  }
  
  DependencyGraphImpl::LoadResult loadFromPath(const driver::Job *, StringRef,
                                               DiagnosticEngine &);

  /// For the dot file.
  std::string getGraphID() const { return "driver"; }

  void forEachUseOf(const ModuleDepGraphNode *def,
                    function_ref<void(const ModuleDepGraphNode *use)>);

  void forEachNode(function_ref<void(const ModuleDepGraphNode *)>) const;

  void forEachArc(function_ref<void(const ModuleDepGraphNode *def,
                                    const ModuleDepGraphNode *use)>) const;

  /// Call \p fn for each node whose key matches \p key.
  void
  forEachMatchingNode(const DependencyKey &key,
                      function_ref<void(const ModuleDepGraphNode *)>) const;

public:
  // This section contains the interface to the status quo code in the driver.

  /// Interface to status quo code in the driver.
  bool isMarked(const driver::Job *) const;

  /// Visit closure of every use of \p job, adding each to visited.
  /// Record any "cascading" nodes visited.
  /// "Cascading" means has a use by an interface in another file.
  void markTransitive(
      SmallVectorImpl<const driver::Job *> &visited, const driver::Job *node,
      DependencyGraph<const driver::Job *>::MarkTracer *tracer = nullptr);

  /// "Mark" this node only.
  bool markIntransitive(const driver::Job *);

  /// Record a new (to this graph) Job.
  void addIndependentNode(const driver::Job *);

  std::vector<std::string> getExternalDependencies() const;

  void markExternal(SmallVectorImpl<const driver::Job *> &uses,
                    StringRef externalDependency);

  /// Return true or abort
  bool verify() const;

  /// Don't want to do this after every integration--too slow--
  /// So export this hook to the driver.
  bool emitAndVerify(DiagnosticEngine &);

private:
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
  void verifyEachJobIsTracked() const;

  static bool mapCorruption(const char *msg) {
    llvm_unreachable(msg);
  }

  /// Use the known swiftDeps to find a directory for
  /// the job-independent dot file.
  std::string computePathForDotFile() const;

  /// Read a SourceFileDepGraph belonging to \p job from \p buffer
  /// and integrate it into the ModuleDepGraph.
  /// Used both the first time, and to reload the SourceFileDepGraph.
  /// If any changes were observed, indicate same in the return vale.
  DependencyGraphImpl::LoadResult loadFromBuffer(const driver::Job *,
                                                 llvm::MemoryBuffer &);

  /// Integrate a SourceFileDepGraph into the receiver.
  /// Integration happens when the driver needs to read SourceFileDepGraph.
  DependencyGraphImpl::LoadResult integrate(const SourceFileDepGraph &);

  /// Integrate the \p integrand into the receiver.
  /// Return a bool indicating if this node represents a change that must be
  /// propagated.
  bool integrateSourceFileDepGraphNode(
      const SourceFileDepGraphNode *integrand,
      StringRef swiftDepsOfSourceFileGraph,
      Optional<ModuleDepGraphNode *> preexistingNodeInPlace);

  /// Integrate the \p integrand, a node that represents a Decl in the swiftDeps
  /// file being integrated. \p preexistingNodeInPlace holds the node
  /// representing the same Decl that already exists, if there is one. \p
  /// prexisintExpat holds a node with the same key that already exists, but was
  /// not known to reside in any swiftDeps file. Return a bool indicating if
  /// this node represents a change that must be propagated.
  bool integrateFrontendDeclNode(
      const SourceFileDepGraphNode *integrand,
      StringRef swiftDepsOfSourceFileGraph,
      Optional<ModuleDepGraphNode *> preexistingNodeInSameFile,
      Optional<ModuleDepGraphNode *> preexistingExpat);

  /// Integrate the \p integrand, a node that was not known to reside in any
  /// swiftDeps file. \p preexistingNodeInSameFile holds the node representing
  /// the same Decl that already exists, if there is one. \p prexisintExpat
  /// holds a node with the same key that already exists, but was not known to
  /// reside in any swiftDeps file. \p dupsExistInOtherFiles is true if there
  /// exists a node with the same key that is known to reside in some other
  /// swiftDeps file. Return a bool indicating if this node represents a change
  /// that must be propagated.
  bool integrateFrontendExpatNode(
      const SourceFileDepGraphNode *integrand,
      Optional<ModuleDepGraphNode *> preexistingNodeInSameFile,
      Optional<ModuleDepGraphNode *> preexistingExpat,
      bool dupsExistInOtherFiles);

  /// Create a brand-new ModuleDepGraphNode to integrate \p integrand.
  ModuleDepGraphNode *
  integrateByCreatingANewNode(const SourceFileDepGraphNode *integrand,
                              Optional<std::string> swiftDepsForNewNode);

  /// Integrate the dependencies of \p integrand which resides in \p
  /// integrandGraph into \p this.
  void integrateUsesByDef(const SourceFileDepGraphNode *integrand,
                          const SourceFileDepGraph &integrandGraph);

  /// If the programmer removes a Decl from a source file, the corresponding
  /// ModuleDepGraphNode needs to be removed.
  void removeNode(ModuleDepGraphNode *);

  /// Starting with the uses of \p potentiallyCascadingDef,
  /// find any newly-cascading jobs.
  /// TODO: stop at marked jobs
  void checkTransitiveClosureForCascading(
      std::unordered_set<const ModuleDepGraphNode *> &visited,
      const ModuleDepGraphNode *potentiallyCascadingDef);

  void rememberThatJobCascades(StringRef swiftDeps) {
    cascadingJobs.insert(swiftDeps);
  }

  /// For debugging, write out the graph to a dot file.
  /// \p diags may be null if no diagnostics are needed.
  void emitDotFileForJob(DiagnosticEngine &, const driver::Job *);
  void emitDotFile(DiagnosticEngine &, StringRef baseName);
  void emitDotFile() { emitDotFile(llvm::errs()); }
  void emitDotFile(llvm::raw_ostream &);

  bool ensureJobIsTracked(const std::string &swiftDeps) const {
    assert(swiftDeps.empty() || getJob(swiftDeps));
    return true;
  }
};
} // namespace experimental_dependencies
} // namespace swift

#endif /* ExperimentalDependencyGraph_h */
