//===--- PackConformance.cpp - Variadic Protocol Conformance --------------===//
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
// This file implements the PackConformance structure, which describes the
// conformance of a type pack parameter to a protocol.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/PackConformance.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/InFlightSubstitution.h"
#include "swift/AST/Module.h"
#include "swift/AST/Types.h"
#include "swift/Basic/Assertions.h"

#define DEBUG_TYPE "AST"

using namespace swift;

void PackConformance::Profile(llvm::FoldingSetNodeID &ID) const {
  Profile(ID, ConformingType, Protocol, getPatternConformances());
}

void PackConformance::Profile(llvm::FoldingSetNodeID &ID,
                              PackType *conformingType,
                              ProtocolDecl *protocol,
                              ArrayRef<ProtocolConformanceRef> conformances) {
  ID.AddPointer(conformingType);
  ID.AddPointer(protocol);
  for (auto conformance : conformances)
    ID.AddPointer(conformance.getOpaqueValue());
}

PackConformance::PackConformance(PackType *conformingType,
                                 ProtocolDecl *protocol,
                                 ArrayRef<ProtocolConformanceRef> conformances)
  : ConformingType(conformingType), Protocol(protocol) {

  assert(ConformingType->getNumElements() == conformances.size());
  std::uninitialized_copy(conformances.begin(), conformances.end(),
                          getTrailingObjects<ProtocolConformanceRef>());
}

size_t PackConformance::numTrailingObjects(
    OverloadToken<ProtocolConformanceRef>) const {
  return ConformingType->getNumElements();
}

bool PackConformance::isInvalid() const {
  return llvm::any_of(getPatternConformances(),
                      [&](const auto ref) { return ref.isInvalid(); });
}

ArrayRef<ProtocolConformanceRef>
PackConformance::getPatternConformances() const {
  return {getTrailingObjects<ProtocolConformanceRef>(),
          ConformingType->getNumElements()};
}

bool PackConformance::isCanonical() const {
  if (!ConformingType->isCanonical())
    return false;

  for (auto conformance : getPatternConformances())
    if (!conformance.isCanonical())
      return false;

  return true;
}

PackConformance *PackConformance::getCanonicalConformance() const {
  if (isCanonical())
    return const_cast<PackConformance *>(this);

  SmallVector<ProtocolConformanceRef, 4> conformances;
  for (auto conformance : getPatternConformances())
    conformances.push_back(conformance.getCanonicalConformanceRef());

  auto canonical = PackConformance::get(
      cast<PackType>(ConformingType->getCanonicalType()),
      Protocol, conformances);

  assert(canonical->isCanonical());
  return canonical;
}

/// Project the corresponding associated type from each pack element
/// of the conforming type, collecting the results into a new pack type
/// that has the same pack expansion structure as the conforming type.
PackType *PackConformance::getAssociatedType(Type assocType) const {
  SmallVector<Type, 4> packElements;

  auto conformances = getPatternConformances();
  for (unsigned i : indices(conformances)) {
    auto packElement = ConformingType->getElementType(i);

    // If the pack element is a pack expansion, build a new pack expansion
    // with the same count type as the original element, and the pattern
    // type replaced with the associated type witness from the pattern
    // conformance.
    if (auto *packExpansion = packElement->getAs<PackExpansionType>()) {
      auto assocTypePattern =
        conformances[i].getAssociatedType(packExpansion->getPatternType(),
                                          assocType);

      packElements.push_back(PackExpansionType::get(
          assocTypePattern, packExpansion->getCountType()));

    // If the pack element is a scalar type, replace the scalar type with
    // the associated type witness from the pattern conformance.
    } else {
      auto assocTypeScalar =
        conformances[i].getAssociatedType(packElement, assocType);
      packElements.push_back(assocTypeScalar);
    }
  }

  return PackType::get(Protocol->getASTContext(), packElements);
}

/// Project the corresponding associated conformance from each pack element
/// of the conforming type, collecting the results into a new pack conformnace
/// whose conforming type has the same pack expansion structure as our
/// conforming type.
PackConformance *PackConformance::getAssociatedConformance(
    Type assocType, ProtocolDecl *protocol) const {
  SmallVector<Type, 4> packElements;
  SmallVector<ProtocolConformanceRef, 4> packConformances;

  auto conformances = getPatternConformances();
  for (unsigned i : indices(conformances)) {
    auto packElement = ConformingType->getElementType(i);

    if (auto *packExpansion = packElement->getAs<PackExpansionType>()) {
      auto assocTypePattern =
        conformances[i].getAssociatedType(packExpansion->getPatternType(),
                                          assocType);
      packElements.push_back(PackExpansionType::get(
          assocTypePattern, packExpansion->getCountType()));

      auto assocConformancePattern =
        conformances[i].getAssociatedConformance(packExpansion->getPatternType(),
                                                 assocType, protocol);
      packConformances.push_back(assocConformancePattern);
    } else {
      auto assocTypeScalar =
        conformances[i].getAssociatedType(packElement, assocType);
      packElements.push_back(assocTypeScalar);

      auto assocConformanceScalar =
        conformances[i].getAssociatedConformance(packElement, assocType, protocol);
      packConformances.push_back(assocConformanceScalar);
    }
  }

  auto conformingType = PackType::get(Protocol->getASTContext(), packElements);
  return PackConformance::get(conformingType, protocol, packConformances);
}

ProtocolConformanceRef PackConformance::subst(SubstitutionMap subMap,
                                              SubstOptions options) const {
  InFlightSubstitutionViaSubMap IFS(subMap, options);
  return subst(IFS);
}

ProtocolConformanceRef PackConformance::subst(TypeSubstitutionFn subs,
                                              LookupConformanceFn conformances,
                                              SubstOptions options) const {
  InFlightSubstitution IFS(subs, conformances, options);
  return subst(IFS);
}

ProtocolConformanceRef
PackConformance::subst(InFlightSubstitution &IFS) const {
  // Results built up by the expansion.
  SmallVector<Type, 4> substElementTypes;
  SmallVector<ProtocolConformanceRef, 4> substConformances;

  auto origConformances = getPatternConformances();
  assert(ConformingType->getNumElements() == origConformances.size());

  for (auto i : range(ConformingType->getNumElements())) {
    auto origElementType = ConformingType->getElementType(i);
    if (auto *origExpansion = origElementType->getAs<PackExpansionType>()) {
      // Substitute and expand an expansion element of the original pack.
      IFS.expandPackExpansionType(origExpansion,
                                  [&](Type substComponentType) {
        substElementTypes.push_back(substComponentType);

        // Just substitute the conformance.  We don't directly represent
        // pack expansion conformances here; it's sort of implicit in the
        // corresponding pack element type.
        substConformances.push_back(
            origConformances[i].subst(origExpansion->getPatternType(), IFS));
      });
    } else {
      // Substitute a scalar element of the original pack.
      substElementTypes.push_back(origElementType.subst(IFS));

      substConformances.push_back(
          origConformances[i].subst(origElementType, IFS));
    }
  }

  auto &ctx = Protocol->getASTContext();
  auto *substConformingType = PackType::get(ctx, substElementTypes);

  auto substConformance = PackConformance::get(substConformingType, Protocol,
                                               substConformances);
  return ProtocolConformanceRef(substConformance);
}

void swift::simple_display(llvm::raw_ostream &out, PackConformance *conformance) {
  out << conformance->getType() << " : {";
  bool first = true;
  for (auto patternConformance : conformance->getPatternConformances()) {
    if (first) {
      out << ", ";
      first = false;
    }
    simple_display(out, patternConformance);
  }
  out << "}";
}
