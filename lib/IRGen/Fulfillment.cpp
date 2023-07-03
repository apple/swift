//===--- Fulfillment.cpp - Static metadata search  ------------------------===//
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
//
//  This file implements routines for searching for ways to find metadata
//  from other metadata.
//
//===----------------------------------------------------------------------===//

#include "Fulfillment.h"
#include "IRGenModule.h"

#include "GenericRequirement.h"
#include "MetadataRequest.h"
#include "ProtocolInfo.h"
#include "swift/AST/Decl.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/SIL/SILWitnessTable.h"
#include "swift/SIL/TypeLowering.h"

using namespace swift;
using namespace irgen;

/// Is metadata for the given type kind a "leaf", or does it possibly
/// store any other type metadata that we can statically extract?
///
/// It's okay to conservatively answer "no".  It's more important for this
/// to be quick than for it to be accurate; don't recurse.
static bool isLeafTypeMetadata(CanType type) {
  switch (type->getKind()) {
#define SUGARED_TYPE(ID, SUPER) \
  case TypeKind::ID:
#define UNCHECKED_TYPE(ID, SUPER) \
  case TypeKind::ID:
#define TYPE(ID, SUPER)
#include "swift/AST/TypeNodes.def"
  case TypeKind::Error:
    llvm_unreachable("kind is invalid for a canonical type");

#define ARTIFICIAL_TYPE(ID, SUPER) \
  case TypeKind::ID:
#define TYPE(ID, SUPER)
#include "swift/AST/TypeNodes.def"
  case TypeKind::LValue:
  case TypeKind::InOut:
  case TypeKind::DynamicSelf:
  case TypeKind::PackExpansion:
  case TypeKind::PackElement:
  case TypeKind::BuiltinTuple:
    llvm_unreachable("these types do not have metadata");

  // All the builtin types are leaves.
#define BUILTIN_TYPE(ID, SUPER) \
  case TypeKind::ID:
#define TYPE(ID, SUPER)
#include "swift/AST/TypeNodes.def"
  case TypeKind::Module:
    return true;

  // Type parameters are statically opaque.
  case TypeKind::PrimaryArchetype:
  case TypeKind::OpenedArchetype:
  case TypeKind::OpaqueTypeArchetype:
  case TypeKind::PackArchetype:
  case TypeKind::ElementArchetype:
  case TypeKind::GenericTypeParam:
  case TypeKind::DependentMember:
    return true;

  // Only the empty tuple is a leaf.
  case TypeKind::Tuple:
    return cast<TupleType>(type)->getNumElements() == 0;

  case TypeKind::Pack:
    return cast<PackType>(type)->getNumElements() == 0;

  // Nominal types might have generic parents.
  case TypeKind::Class:
  case TypeKind::Enum:
  case TypeKind::Protocol:
  case TypeKind::Struct:
    return !cast<NominalType>(type)->getDecl()->isGenericContext();

  // Bound generic types have type arguments.
  case TypeKind::BoundGenericClass:
  case TypeKind::BoundGenericEnum:
  case TypeKind::BoundGenericStruct:
    return false;

  // Functions have component types.
  case TypeKind::Function:
  case TypeKind::GenericFunction:  // included for future-proofing
    return false;

  // Protocol compositions have component types.
  case TypeKind::ProtocolComposition:
    return false;

  // Parametrized protocols have component types.
  case TypeKind::ParameterizedProtocol:
    return false;

  // Existential types have constraint types.
  case TypeKind::Existential:
    return false;

  // Metatypes have instance types.
  case TypeKind::Metatype:
  case TypeKind::ExistentialMetatype:
    return false;
  }
  llvm_unreachable("bad type kind");
}

/// Given that we have a source for metadata of the given type, check
/// to see if it fulfills anything.
///
/// \param isExact - true if the metadata is known to be exactly the
///   metadata for the given type, false if it might be a subtype
bool FulfillmentMap::searchTypeMetadata(IRGenModule &IGM, CanType type,
                                        IsExact_t isExact,
                                        MetadataState metadataState,
                                        unsigned source, MetadataPath &&path,
                                        const InterestingKeysCallback &keys) {

  // If this is an exact source, and it's an interesting type, add this
  // as a fulfillment.
  if (isExact && keys.isInterestingType(type)) {
    // If the type isn't a leaf type, also check it as an inexact match.
    bool hadFulfillment = false;
    if (!isLeafTypeMetadata(type)) {
      hadFulfillment |= searchTypeMetadata(IGM, type, IsInexact, metadataState,
                                           source, MetadataPath(path), keys);
    }

    // Consider its super class bound.
    if (metadataState == MetadataState::Complete) {
      if (auto superclassTy = keys.getSuperclassBound(type)) {
        hadFulfillment |= searchNominalTypeMetadata(
            IGM, superclassTy, metadataState, source, std::move(path), keys);
      }
    }

    // Add the fulfillment.
    hadFulfillment |= addFulfillment(GenericRequirement::forMetadata(type),
                                     source, std::move(path), metadataState);
    return hadFulfillment;
  }

  // Search the superclass fields.  We can only do this if the metadata
  // is complete.
  if (metadataState == MetadataState::Complete &&
      keys.isInterestingType(type)) {
    if (auto superclassTy = keys.getSuperclassBound(type)) {
      return searchNominalTypeMetadata(IGM, superclassTy, metadataState,
                                       source, std::move(path), keys);
    }
  }

  // Inexact metadata will be a problem if we ever try to use this
  // to remember that we already have the metadata for something.
  if (isa<NominalType>(type) || isa<BoundGenericType>(type)) {
    return searchNominalTypeMetadata(IGM, type, metadataState,
                                     source, std::move(path), keys);
  }

  // TODO: tuples
  // TODO: functions
  // TODO: metatypes

  return false;
}

static CanType getSingletonPackExpansionParameter(
    CanPackType packType, const FulfillmentMap::InterestingKeysCallback &keys,
    llvm::Optional<unsigned> &packExpansionComponent) {
  if (auto expansion = packType.unwrapSingletonPackExpansion()) {
    if (keys.isInterestingPackExpansion(expansion)) {
      packExpansionComponent = 0;
      return expansion.getPatternType();
    }
  }

  return CanType();
}

bool FulfillmentMap::searchTypeMetadataPack(IRGenModule &IGM,
                                            CanPackType packType,
                                            IsExact_t isExact,
                                            MetadataState metadataState,
                                            unsigned source,
                                            MetadataPath &&path,
                                      const InterestingKeysCallback &keys) {
  // We can fulfill pack parameters if the pack is a singleton pack
  // expansion over one.
  // TODO: we can also fulfill pack expansions if we can slice away
  // constant-sized prefixes and suffixes.
  llvm::Optional<unsigned> packExpansionComponent;
  if (auto parameter = getSingletonPackExpansionParameter(packType, keys,
                                                    packExpansionComponent)) {
    MetadataPath singletonPath = path;
    singletonPath.addPackExpansionPatternComponent(*packExpansionComponent);
    return addFulfillment(GenericRequirement::forMetadata(parameter),
                          source, std::move(singletonPath), metadataState);
  }

  // TODO: fulfill non-expansion metadata out of the pack

  // TODO: fulfill the pack type itself

  return false;
}

bool FulfillmentMap::searchConformance(
    IRGenModule &IGM, const ProtocolConformance *conformance,
    unsigned sourceIndex, MetadataPath &&path,
    const InterestingKeysCallback &interestingKeys) {
  bool hadFulfillment = false;

  SILWitnessTable::enumerateWitnessTableConditionalConformances(
      conformance, [&](unsigned index, CanType type, ProtocolDecl *protocol) {
        llvm::Optional<unsigned> packExpansionComponent;

        if (auto packType = dyn_cast<PackType>(type)) {
          auto param =
              getSingletonPackExpansionParameter(packType, interestingKeys,
                                                 packExpansionComponent);
          if (!param)
            return /*finished?*/ false;
          type = param;
        }

        MetadataPath conditionalPath = path;
        conditionalPath.addConditionalConformanceComponent(index);
        if (packExpansionComponent)
          conditionalPath.addPackExpansionPatternComponent(*packExpansionComponent);

        hadFulfillment |=
            searchWitnessTable(IGM, type, protocol, sourceIndex,
                               std::move(conditionalPath), interestingKeys);

        return /*finished?*/ false;
      });

  return hadFulfillment;
}

bool FulfillmentMap::searchWitnessTable(IRGenModule &IGM,
                                        CanType type, ProtocolDecl *protocol,
                                        unsigned source, MetadataPath &&path,
                                        const InterestingKeysCallback &keys) {
  assert(Lowering::TypeConverter::protocolRequiresWitnessTable(protocol));

  llvm::SmallPtrSet<ProtocolDecl*, 4> interestingConformancesBuffer;
  llvm::SmallPtrSetImpl<ProtocolDecl *> *interestingConformances = nullptr;

  // If the interesting-keys set is limiting the set of interesting
  // conformances, collect that filter.
  if (keys.hasInterestingType(type) &&
      keys.hasLimitedInterestingConformances(type)) {
    // Bail out immediately if the set is empty.
    // This only makes sense because we're not trying to fulfill
    // associated types this way.
    auto requiredConformances = keys.getInterestingConformances(type);
    if (requiredConformances.empty()) return false;

    interestingConformancesBuffer.insert(requiredConformances.begin(),
                                         requiredConformances.end());
    interestingConformances = &interestingConformancesBuffer;
  }

  return searchWitnessTable(IGM, type, protocol, source, std::move(path), keys,
                            interestingConformances);
}

bool FulfillmentMap::searchWitnessTable(
    IRGenModule &IGM, CanType type, ProtocolDecl *protocol, unsigned source,
    MetadataPath &&path, const InterestingKeysCallback &keys,
    llvm::SmallPtrSetImpl<ProtocolDecl *> *interestingConformances) {

  bool hadFulfillment = false;

  auto &pi = IGM.getProtocolInfo(protocol,
                                 ProtocolInfoKind::RequirementSignature);

  for (auto &entry : pi.getWitnessEntries()) {
    if (!entry.isBase()) continue;

    ProtocolDecl *inherited = entry.getBase();
    MetadataPath inheritedPath = path;
    inheritedPath.addInheritedProtocolComponent(pi.getBaseWitnessIndex(&entry));
    hadFulfillment |= searchWitnessTable(IGM, type, inherited,
                                         source, std::move(inheritedPath),
                                         keys, interestingConformances);
  }

  // If we're not limiting the set of interesting conformances, or if
  // this is an interesting conformance, record it.
  if (!interestingConformances || interestingConformances->count(protocol)) {
    hadFulfillment |= addFulfillment(
        GenericRequirement::forWitnessTable(type, protocol), source,
        std::move(path), MetadataState::Complete);
  }

  return hadFulfillment;
}


bool FulfillmentMap::searchNominalTypeMetadata(IRGenModule &IGM,
                                               CanType type,
                                               MetadataState metadataState,
                                               unsigned source,
                                               MetadataPath &&path,
                                         const InterestingKeysCallback &keys) {
  // Objective-C generics don't preserve their generic parameters at runtime,
  // so they aren't able to fulfill type metadata requirements.
  if (type.getAnyNominal()->hasClangNode()) {
    return false;
  }
  
  auto *nominal = type.getAnyNominal();
  if (!nominal->isGenericContext() || isa<ProtocolDecl>(nominal)) {
    return false;
  }

  bool hadFulfillment = false;

  auto subs = type->getContextSubstitutionMap(IGM.getSwiftModule(), nominal);

  GenericTypeRequirements requirements(IGM, nominal);

  for (unsigned reqtIndex : indices(requirements.getRequirements())) {
    auto requirement = requirements.getRequirements()[reqtIndex];
    auto arg = requirement.getTypeParameter().subst(subs)->getCanonicalType();

    // Skip uninteresting type arguments.
    if (!keys.hasInterestingType(arg))
      continue;

    switch (requirement.getKind()) {
    case GenericRequirement::Kind::Shape: {
      // If the fulfilled value is a shape class, refine the path.
      MetadataPath argPath = path;
      argPath.addNominalTypeArgumentShapeComponent(reqtIndex);

      hadFulfillment |= searchShapeRequirement(IGM, arg, source,
                                               std::move(argPath));
      break;
    }
    case GenericRequirement::Kind::Metadata:
    case GenericRequirement::Kind::MetadataPack: {
      // If the fulfilled value is type metadata, refine the path.
      auto argState =
          getPresumedMetadataStateForTypeArgument(metadataState);
      MetadataPath argPath = path;
      argPath.addNominalTypeArgumentComponent(reqtIndex);

      if (requirement.getKind() == GenericRequirement::Kind::Metadata)
        hadFulfillment |=
          searchTypeMetadata(IGM, arg, IsExact, argState,
                              source, std::move(argPath), keys);
      else
        hadFulfillment |=
          searchTypeMetadataPack(IGM, cast<PackType>(arg), IsExact, argState,
                                 source, std::move(argPath), keys);
      break;
    }
    case GenericRequirement::Kind::WitnessTablePack:
    case GenericRequirement::Kind::WitnessTable: {
      llvm::Optional<unsigned> packExpansionComponent;
      if (requirement.getKind() == GenericRequirement::Kind::WitnessTable) {
        // Ignore it unless the type itself is interesting.
        if (!keys.isInterestingType(arg))
          continue;
      } else {
        // Ignore it unless the pack is a singleton pack expansion of a
        // type parameter, in which case use that type below.
        auto param =
            getSingletonPackExpansionParameter(cast<PackType>(arg), keys,
                                               packExpansionComponent);
        if (!param) continue;
        arg = param;
      }

      // Refine the path.
      MetadataPath argPath = path;
      argPath.addNominalTypeArgumentConformanceComponent(reqtIndex);
      if (packExpansionComponent)
        argPath.addPackExpansionPatternComponent(*packExpansionComponent);

      // This code just handles packs directly.
      hadFulfillment |=
        searchWitnessTable(IGM, arg, requirement.getProtocol(),
                           source, std::move(argPath), keys);
      break;
    }
    }
  }

  return hadFulfillment;
}

bool FulfillmentMap::searchShapeRequirement(IRGenModule &IGM, CanType argType,
                                            unsigned source, MetadataPath &&path) {
  // argType is the substitution for a pack parameter, so it should always
  // be a pack.
  auto packType = cast<PackType>(argType);

  // For now, don't try to fulfill shapes if this isn't a singleton
  // pack containing a pack expansion.  In theory, though, as long as
  // there aren't expansions over pack parameters with different shapes,
  // we should always be able to turn this into the equation
  // `ax + b = <fulfilled count>` and then solve that.
  auto expansion = packType.unwrapSingletonPackExpansion();
  if (!expansion)
    return false;

  path.addPackExpansionCountComponent(0);

  auto parameter = expansion.getCountType();
  
  // Add the fulfillment.
  return addFulfillment(GenericRequirement::forShape(parameter),
                        source, std::move(path), MetadataState::Complete);
}

/// Testify that there's a fulfillment at the given path.
bool FulfillmentMap::addFulfillment(GenericRequirement key,
                                    unsigned source,
                                    MetadataPath &&path,
                                    MetadataState metadataState) {
  // Only add a fulfillment if we don't have any previous
  // fulfillment for that value or if it 's cheaper than the existing
  // fulfillment.
  auto it = Fulfillments.find(key);
  if (it != Fulfillments.end()) {
    // If the new fulfillment is worse than the existing one, ignore it.
    auto existingState = it->second.getState();
    if (!isAtLeast(metadataState, existingState)) {
      return false;
    }

    // Consider cost only if the fulfillments are equivalent in state.
    // TODO: this is potentially suboptimal, but it generally won't matter.
    if (metadataState == existingState && 
        path.cost() >= it->second.Path.cost()) {
      return false;
    }

    it->second.SourceIndex = source;
    it->second.Path = std::move(path);
    return true;
  } else {
    Fulfillments.insert({ key, Fulfillment(source, std::move(path),
                                           metadataState) });
    return true;
  }
}

static StringRef getStateName(MetadataState state) {
  switch (state) {
  case MetadataState::Complete: return "complete";
  case MetadataState::NonTransitiveComplete: return "non-transitive-complete";
  case MetadataState::LayoutComplete: return "layout-complete";
  case MetadataState::Abstract: return "abstract";
  }
  llvm_unreachable("unhandled state");
}

void FulfillmentMap::dump() const {
  auto &out = llvm::errs();
  for (auto &entry : Fulfillments) {
    out << "(";
    entry.first.dump(out);
    out << ") => " << getStateName(entry.second.getState())
        << " at sources[" << entry.second.SourceIndex
        << "]." << entry.second.Path << "\n";
  }
}
