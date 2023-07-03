//===--- SILProfiler.h - Instrumentation based profiling ===-----*- C++ -*-===//
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
//
// This file defines SILProfiler, which contains the profiling state for one
// function. It's used to drive PGO and generate code coverage reports.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SIL_PROFILER_H
#define SWIFT_SIL_PROFILER_H

#include "swift/AST/ASTNode.h"
#include "swift/Basic/ProfileCounter.h"
#include "swift/SIL/SILAllocated.h"
#include "swift/SIL/SILDeclRef.h"
#include "llvm/ADT/DenseMap.h"

namespace swift {

class AbstractFunctionDecl;
class SILCoverageMap;
class SILFunction;
class SILModule;

/// A reference to a given profiler counter.
class ProfileCounterRef final {
public:
  enum class Kind : uint8_t {
    /// References an ASTNode.
    // TODO: This is the currently the only kind, but it will be expanded in
    // the future for e.g top-level entry and error branches.
    Node
  };

private:
  friend struct llvm::DenseMapInfo<ProfileCounterRef>;

  ASTNode Node;
  Kind RefKind;

  ProfileCounterRef(ASTNode node, Kind kind) : Node(node), RefKind(kind) {}

public:
  /// A profile counter that is associated with a given ASTNode.
  static ProfileCounterRef node(ASTNode node) {
    assert(node && "Must have non-null ASTNode");
    return ProfileCounterRef(node, Kind::Node);
  }

  /// Retrieve the corresponding location of the counter.
  SILLocation getLocation() const;

  SWIFT_DEBUG_DUMP;
  void dump(raw_ostream &OS) const;

  /// A simpler dump output for inline printing.
  void dumpSimple(raw_ostream &OS) const;

  friend bool operator==(const ProfileCounterRef &lhs,
                         const ProfileCounterRef &rhs) {
    return lhs.Node == rhs.Node && lhs.RefKind == rhs.RefKind;
  }
  friend bool operator!=(const ProfileCounterRef &lhs,
                         const ProfileCounterRef &rhs) {
    return !(lhs == rhs);
  }
  friend llvm::hash_code hash_value(const ProfileCounterRef &ref) {
    return llvm::hash_combine(ref.Node, ref.RefKind);
  }
};

/// SILProfiler - Maps AST nodes to profile counters.
class SILProfiler : public SILAllocated<SILProfiler> {
private:
  SILModule &M;

  SILDeclRef forDecl;

  bool EmitCoverageMapping;

  SILCoverageMap *CovMap = nullptr;

  StringRef CurrentFileName;

  std::string PGOFuncName;

  uint64_t PGOFuncHash = 0;

  unsigned NumRegionCounters = 0;

  llvm::DenseMap<ProfileCounterRef, unsigned> RegionCounterMap;

  llvm::DenseMap<ProfileCounterRef, ProfileCounter> RegionLoadedCounterMap;

  llvm::DenseMap<ASTNode, ASTNode> RegionCondToParentMap;

  std::vector<std::tuple<std::string, uint64_t, std::string>> CoverageData;

  SILProfiler(SILModule &M, SILDeclRef forDecl, bool EmitCoverageMapping)
      : M(M), forDecl(forDecl), EmitCoverageMapping(EmitCoverageMapping) {}

public:
  static SILProfiler *create(SILModule &M, SILDeclRef Ref);

  /// Check if the function is set up for profiling.
  bool hasRegionCounters() const { return NumRegionCounters != 0; }

  /// Get the execution count corresponding to \p Ref from a profile, if one
  /// is available.
  ProfileCounter getExecutionCount(ProfileCounterRef Ref);

  /// Get the execution count corresponding to \p Node from a profile, if one
  /// is available.
  ProfileCounter getExecutionCount(ASTNode Node);

  /// Get the node's parent ASTNode (e.g to get the parent IfStmt or IfCond of
  /// a condition), if one is available.
  llvm::Optional<ASTNode> getPGOParent(ASTNode Node);

  /// Get the function name mangled for use with PGO.
  StringRef getPGOFuncName() const { return PGOFuncName; }

  /// Get the function hash.
  uint64_t getPGOFuncHash() const { return PGOFuncHash; }

  /// Get the number of region counters.
  unsigned getNumRegionCounters() const { return NumRegionCounters; }

  /// Get the mapping from a \c ProfileCounterRef to its corresponding
  /// profile counter.
  const llvm::DenseMap<ProfileCounterRef, unsigned> &
  getRegionCounterMap() const {
    return RegionCounterMap;
  }

private:
  /// Map counters to ASTNodes and set them up for profiling the function.
  void assignRegionCounters();
};

} // end namespace swift

namespace llvm {
using swift::ProfileCounterRef;
using swift::ASTNode;

template <> struct DenseMapInfo<ProfileCounterRef> {
  static inline ProfileCounterRef getEmptyKey() {
    return ProfileCounterRef(DenseMapInfo<ASTNode>::getEmptyKey(),
                             ProfileCounterRef::Kind::Node);
  }
  static inline ProfileCounterRef getTombstoneKey() {
    return ProfileCounterRef(DenseMapInfo<ASTNode>::getTombstoneKey(),
                             ProfileCounterRef::Kind::Node);
  }
  static unsigned getHashValue(const ProfileCounterRef &ref) {
    return hash_value(ref);
  }
  static bool isEqual(const ProfileCounterRef &lhs,
                      const ProfileCounterRef &rhs) {
    return lhs == rhs;
  }
};
} // end namespace llvm

#endif // SWIFT_SIL_PROFILER_H
