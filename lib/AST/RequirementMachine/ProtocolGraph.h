//===--- ProtocolGraph.h - Collects information about protocols -*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_PROTOCOLGRAPH_H
#define SWIFT_PROTOCOLGRAPH_H

#include "swift/AST/Requirement.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/TinyPtrVector.h"

namespace swift {

class ProtocolDecl;
class AssociatedTypeDecl;

namespace rewriting {

/// Stores cached information about a protocol.
struct ProtocolInfo {
  /// All immediately-inherited protocols.
  ArrayRef<ProtocolDecl *> Inherited;

  /// Transitive closure of inherited protocols; does not include the protocol
  /// itself. Computed by ProtocolGraph::computeInheritedProtocols().
  llvm::TinyPtrVector<const ProtocolDecl *> AllInherited;

  /// Associated types defined in the protocol itself.
  llvm::TinyPtrVector<AssociatedTypeDecl *> AssociatedTypes;

  /// Associated types from all inherited protocols, not including duplicates or
  /// those defined in the protocol itself. Computed by
  /// ProtocolGraph::computeInheritedAssociatedTypes().
  llvm::TinyPtrVector<AssociatedTypeDecl *> InheritedAssociatedTypes;

  /// The protocol's requirement signature.
  ArrayRef<Requirement> Requirements;

  /// Used by ProtocolGraph::computeProtocolDepth() to detect circularity.
  unsigned Mark : 1;

  /// Longest chain of protocol refinements, including this one. Greater than
  /// zero on valid code, might be zero if there's a cycle. Computed by
  /// ProtocolGraph::computeLinearOrder().
  unsigned Depth : 31;

  /// Index of the protocol in the linear order. Computed by
  /// ProtocolGraph::computeLinearOrder().
  unsigned Index : 32;

  ProtocolInfo() {
    Mark = 0;
    Depth = 0;
    Index = 0;
  }

  ProtocolInfo(ArrayRef<ProtocolDecl *> inherited,
               llvm::TinyPtrVector<AssociatedTypeDecl *> &&types,
               ArrayRef<Requirement> reqs)
    : Inherited(inherited),
      AssociatedTypes(types),
      Requirements(reqs) {
    Mark = 0;
    Depth = 0;
    Index = 0;
  }
};

/// Stores cached information about all protocols transtively
/// referenced from a set of generic requirements.
///
/// Out-of-line methods are documented in ProtocolGraph.cpp.
class ProtocolGraph {
  llvm::DenseMap<const ProtocolDecl *, ProtocolInfo> Info;
  std::vector<const ProtocolDecl *> Protocols;
  bool Debug = false;

public:
  void visitRequirements(ArrayRef<Requirement> reqs);

  bool isKnownProtocol(const ProtocolDecl *proto) const;

  /// Returns the sorted list of protocols, with the property
  /// that (P refines Q) => P < Q. See compareProtocols()
  /// for details.
  ArrayRef<const ProtocolDecl *> getProtocols() const {
    return Protocols;
  }

  const ProtocolInfo &getProtocolInfo(
      const ProtocolDecl *proto) const;

private:
  void addProtocol(const ProtocolDecl *proto);
  void computeTransitiveClosure();
  void computeLinearOrder();
  void computeInheritedAssociatedTypes();
  void computeInheritedProtocols();

public:
  void compute();

  int compareProtocols(const ProtocolDecl *lhs,
                       const ProtocolDecl *rhs) const;

  bool inheritsFrom(const ProtocolDecl *thisProto,
                    const ProtocolDecl *otherProto) const;

private:
  unsigned computeProtocolDepth(const ProtocolDecl *proto);
};

} // end namespace rewriting

} // end namespace swift

#endif