//===--- ProtocolGraph.cpp - Collect information about protocols ----------===//
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

#include "ProtocolGraph.h"

#include "swift/AST/Decl.h"
#include "swift/AST/Requirement.h"

using namespace swift;
using namespace rewriting;

/// Adds information about all protocols transitvely referenced from
/// \p reqs.
void ProtocolGraph::visitRequirements(ArrayRef<Requirement> reqs) {
  for (auto req : reqs) {
    if (req.getKind() == RequirementKind::Conformance) {
      addProtocol(req.getProtocolDecl());
    }
  }
}

/// Return true if we know about this protocol.
bool ProtocolGraph::isKnownProtocol(const ProtocolDecl *proto) const {
  return Info.count(proto) > 0;
}

/// Look up information about a known protocol.
const ProtocolInfo &ProtocolGraph::getProtocolInfo(
    const ProtocolDecl *proto) const {
  auto found = Info.find(proto);
  assert(found != Info.end());
  return found->second;
}

/// Record information about a protocol if we have no seen it yet.
void ProtocolGraph::addProtocol(const ProtocolDecl *proto) {
  if (Info.count(proto) > 0)
    return;

  Info[proto] = {proto->getInheritedProtocols(),
                 proto->getAssociatedTypeMembers(),
                 proto->getRequirementSignature()};
  Protocols.push_back(proto);
}

/// Record information about all protocols transtively referenced
/// from protocol requirement signatures.
void ProtocolGraph::computeTransitiveClosure() {
  unsigned i = 0;
  while (i < Protocols.size()) {
    auto *proto = Protocols[i++];
    visitRequirements(getProtocolInfo(proto).Requirements);
  }
}

/// See ProtocolGraph::compareProtocols() for the definition of this linear
/// order.
void ProtocolGraph::computeLinearOrder() {
  for (const auto *proto : Protocols) {
    (void) computeProtocolDepth(proto);
  }

  std::sort(
      Protocols.begin(), Protocols.end(),
      [&](const ProtocolDecl *lhs,
          const ProtocolDecl *rhs) -> bool {
        const auto &lhsInfo = getProtocolInfo(lhs);
        const auto &rhsInfo = getProtocolInfo(rhs);

        // protocol Base {} // depth 1
        // protocol Derived : Base {} // depth 2
        //
        // Derived < Base in the linear order.
        if (lhsInfo.Depth != rhsInfo.Depth)
          return lhsInfo.Depth > rhsInfo.Depth;

        return TypeDecl::compare(lhs, rhs) < 0;
      });

  for (unsigned i : indices(Protocols)) {
    Info[Protocols[i]].Index = i;
  }

  if (Debug) {
    for (const auto *proto : Protocols) {
      const auto &info = getProtocolInfo(proto);
      llvm::dbgs() << "@ Protocol " << proto->getName()
                   << " Depth=" << info.Depth
                   << " Index=" << info.Index << "\n";
    }
  }
}

/// Update each ProtocolInfo's AssociatedTypes vector to add all associated
/// types from all transitively inherited protocols.
void ProtocolGraph::computeInheritedAssociatedTypes() {
  // Visit protocols in reverse order, so that if P inherits from Q and
  // Q inherits from R, we first visit R, then Q, then P, ensuring that
  // R's associated types are added to P's list, etc.
  for (const auto *proto : llvm::reverse(Protocols)) {
    auto &info = Info[proto];

    for (const auto *inherited : info.AllInherited) {
      for (auto *inheritedType : getProtocolInfo(inherited).AssociatedTypes) {
        info.InheritedAssociatedTypes.push_back(inheritedType);
      }
    }
  }
}

// Update each protocol's AllInherited vector to add all transitively
// inherited protocols.
void ProtocolGraph::computeInheritedProtocols() {
  // Visit protocols in reverse order, so that if P inherits from Q and
  // Q inherits from R, we first visit R, then Q, then P, ensuring that
  // R's inherited protocols are added to P's list, etc.
  for (const auto *proto : llvm::reverse(Protocols)) {
    auto &info = Info[proto];

    // We might inherit the same protocol multiple times due to diamond
    // inheritance, so make sure we only add each protocol once.
    llvm::SmallDenseSet<const ProtocolDecl *, 4> visited;
    visited.insert(proto);

    for (const auto *inherited : info.Inherited) {
      // Add directly-inherited protocols.
      if (!visited.insert(inherited).second)
        continue;
      info.AllInherited.push_back(inherited);

      // Add indirectly-inherited protocols.
      for (auto *inheritedType : getProtocolInfo(inherited).AllInherited) {
        if (!visited.insert(inheritedType).second)
          continue;

        info.AllInherited.push_back(inheritedType);
      }
    }
  }
}

/// Recursively compute the 'depth' of a protocol, which is inductively defined
/// as one greater than the depth of all inherited protocols, with a protocol
/// that does not inherit any other protocol having a depth of one.
unsigned ProtocolGraph::computeProtocolDepth(const ProtocolDecl *proto) {
  auto &info = Info[proto];

  if (info.Mark) {
    // Already computed, or we have a cycle. Cycles are diagnosed
    // elsewhere in the type checker, so we don't have to do
    // anything here.
    return info.Depth;
  }

  info.Mark = true;
  unsigned depth = 0;

  for (auto *inherited : info.Inherited) {
    unsigned inheritedDepth = computeProtocolDepth(inherited);
    depth = std::max(inheritedDepth, depth);
  }

  depth++;

  info.Depth = depth;
  return depth;
}

/// Compute everything in the right order.
void ProtocolGraph::compute() {
  computeTransitiveClosure();
  computeLinearOrder();
  computeInheritedProtocols();
  computeInheritedAssociatedTypes();
}

/// Defines a linear order with the property that if a protocol P inherits
/// from another protocol Q, then P < Q. (The converse cannot be true, since
/// this is a linear order.)
///
/// We first compare the 'depth' of a protocol, which is defined in
/// ProtocolGraph::computeProtocolDepth() above.
///
/// If two protocols have the same depth, the tie is broken by the standard
/// TypeDecl::compare().
int ProtocolGraph::compareProtocols(const ProtocolDecl *lhs,
                                    const ProtocolDecl *rhs) const {
  const auto &infoLHS = getProtocolInfo(lhs);
  const auto &infoRHS = getProtocolInfo(rhs);

  return infoLHS.Index - infoRHS.Index;
}

/// Returns if \p thisProto transitively inherits from \p otherProto.
///
/// The result is false if the two protocols are equal.
bool ProtocolGraph::inheritsFrom(const ProtocolDecl *thisProto,
                                 const ProtocolDecl *otherProto) const {
  const auto &info = getProtocolInfo(thisProto);
  return std::find(info.AllInherited.begin(),
                   info.AllInherited.end(),
                   otherProto) != info.AllInherited.end();
}
