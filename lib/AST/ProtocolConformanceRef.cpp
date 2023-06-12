//===--- ProtocolConformance.cpp - AST Protocol Conformance Reference -----===//
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
// This file implements the ProtocolConformanceRef structure, which wraps a
// concrete or abstract conformance, or is invalid.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ProtocolConformanceRef.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Availability.h"
#include "swift/AST/Decl.h"
#include "swift/AST/InFlightSubstitution.h"
#include "swift/AST/Module.h"
#include "swift/AST/PackConformance.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/AST/Types.h"

#define DEBUG_TYPE "AST"

using namespace swift;

ProtocolConformanceRef::ProtocolConformanceRef(ProtocolDecl *protocol,
                                               ProtocolConformance *conf) {
  assert(protocol != nullptr &&
         "cannot construct ProtocolConformanceRef with null protocol");
  if (conf) {
    assert(protocol == conf->getProtocol() && "protocol conformance mismatch");
    Union = conf;
  } else {
    Union = protocol;
  }
}

bool ProtocolConformanceRef::isInvalid() const {
  if (!Union)
    return true;

  if (auto pack = Union.dyn_cast<PackConformance *>())
    return pack->isInvalid();

  return false;
}

ProtocolDecl *ProtocolConformanceRef::getRequirement() const {
  assert(!isInvalid());

  if (isConcrete()) {
    return getConcrete()->getProtocol();
  } else if (isPack()) {
    return getPack()->getProtocol();
  } else {
    return getAbstract();
  }
}

ProtocolConformanceRef
ProtocolConformanceRef::subst(Type origType,
                              SubstitutionMap subMap,
                              SubstOptions options) const {
  InFlightSubstitutionViaSubMap IFS(subMap, options);
  return subst(origType, IFS);
}

ProtocolConformanceRef
ProtocolConformanceRef::subst(Type origType,
                              TypeSubstitutionFn subs,
                              LookupConformanceFn conformances,
                              SubstOptions options) const {
  InFlightSubstitution IFS(subs, conformances, options);
  return subst(origType, IFS);
}

ProtocolConformanceRef
ProtocolConformanceRef::subst(Type origType, InFlightSubstitution &IFS) const {
  if (isInvalid())
    return *this;

  if (isConcrete())
    return ProtocolConformanceRef(getConcrete()->subst(IFS));
  if (isPack())
    return getPack()->subst(IFS);

  // Handle abstract conformances below:

  // If the type is an opaque archetype, the conformance will remain abstract,
  // unless we're specifically substituting opaque types.
  if (auto origArchetype = origType->getAs<ArchetypeType>()) {
    if (!IFS.shouldSubstituteOpaqueArchetypes()
        && isa<OpaqueTypeArchetypeType>(origArchetype)) {
      return *this;
    }
  }

  // Otherwise, compute the substituted type.
  auto substType = origType.subst(IFS);

  auto *proto = getRequirement();

  // If the type is an existential, it must be self-conforming.
  if (substType->isExistentialType()) {
    auto optConformance =
        proto->getModuleContext()->lookupExistentialConformance(substType,
                                                                proto);
    if (optConformance)
      return optConformance;

    return ProtocolConformanceRef::forInvalid();
  }

  // Check the conformance map.
  // FIXME: Pack element level?
  return IFS.lookupConformance(origType->getCanonicalType(), substType, proto,
                               /*level=*/0);
}

ProtocolConformanceRef ProtocolConformanceRef::mapConformanceOutOfContext() const {
  if (isConcrete()) {
    auto *concrete = getConcrete()->subst(
        [](SubstitutableType *type) -> Type {
          if (auto *archetypeType = type->getAs<ArchetypeType>())
            return archetypeType->getInterfaceType();
          return type;
        },
        MakeAbstractConformanceForGenericType(),
        SubstFlags::PreservePackExpansionLevel);
    return ProtocolConformanceRef(concrete);
  } else if (isPack()) {
    return getPack()->subst(
        [](SubstitutableType *type) -> Type {
          if (auto *archetypeType = type->getAs<ArchetypeType>())
            return archetypeType->getInterfaceType();
          return type;
        },
        MakeAbstractConformanceForGenericType(),
        SubstFlags::PreservePackExpansionLevel);
  }

  return *this;
}

Type
ProtocolConformanceRef::getTypeWitnessByName(Type type, Identifier name) const {
  assert(!isInvalid());

  // Find the named requirement.
  ProtocolDecl *proto = getRequirement();
  auto *assocType = proto->getAssociatedType(name);

  // FIXME: Shouldn't this be a hard error?
  if (!assocType)
    return ErrorType::get(proto->getASTContext());

  return assocType->getDeclaredInterfaceType().subst(
    SubstitutionMap::getProtocolSubstitutions(proto, type, *this));
}

ConcreteDeclRef
ProtocolConformanceRef::getWitnessByName(Type type, DeclName name) const {
  // Find the named requirement.
  auto *proto = getRequirement();
  auto *requirement = proto->getSingleRequirement(name);
  if (requirement == nullptr)
    return ConcreteDeclRef();

  // For a type with dependent conformance, just return the requirement from
  // the protocol. There are no protocol conformance tables.
  if (!isConcrete()) {
    auto subs = SubstitutionMap::getProtocolSubstitutions(proto, type, *this);
    return ConcreteDeclRef(requirement, subs);
  }

  return getConcrete()->getWitnessDeclRef(requirement);
}

Optional<ArrayRef<Requirement>>
ProtocolConformanceRef::getConditionalRequirementsIfAvailable() const {
  if (isConcrete())
    return getConcrete()->getConditionalRequirementsIfAvailable();
  else
    // An abstract conformance is never conditional: any conditionality in the
    // concrete types that will eventually pass through this at runtime is
    // completely pre-checked and packaged up.
    return ArrayRef<Requirement>();
}

ArrayRef<Requirement>
ProtocolConformanceRef::getConditionalRequirements() const {
  if (isConcrete())
    return getConcrete()->getConditionalRequirements();
  else
    // An abstract conformance is never conditional, as above.
    return {};
}

Type ProtocolConformanceRef::getAssociatedType(Type conformingType,
                                               Type assocType) const {
  if (isPack()) {
    auto *pack = getPack();
    assert(conformingType->isEqual(pack->getType()));
    return pack->getAssociatedType(assocType);
  }

  assert(!isConcrete() || getConcrete()->getType()->isEqual(conformingType));

  auto type = assocType->getCanonicalType();
  auto proto = getRequirement();

  // Fast path for generic parameters.
  if (isa<GenericTypeParamType>(type)) {
    assert(type->isEqual(proto->getSelfInterfaceType()) &&
           "type parameter in protocol was not Self");
    return conformingType;
  }

  // Fast path for dependent member types on 'Self' of our associated types.
  auto memberType = cast<DependentMemberType>(type);
  if (memberType.getBase()->isEqual(proto->getSelfInterfaceType()) &&
      memberType->getAssocType()->getProtocol() == proto &&
      isConcrete())
    return getConcrete()->getTypeWitness(memberType->getAssocType());

  // General case: consult the substitution map.
  auto substMap =
    SubstitutionMap::getProtocolSubstitutions(proto, conformingType, *this);
  return type.subst(substMap);
}

ProtocolConformanceRef
ProtocolConformanceRef::getAssociatedConformance(Type conformingType,
                                                 Type assocType,
                                                 ProtocolDecl *protocol) const {
  // If this is a pack conformance, project the associated conformances.
  if (isPack()) {
    auto *pack = getPack();
    assert(conformingType->isEqual(pack->getType()));
    return ProtocolConformanceRef(
        pack->getAssociatedConformance(assocType, protocol));
  }

  // If this is a concrete conformance, look up the associated conformance.
  if (isConcrete()) {
    auto conformance = getConcrete();
    assert(conformance->getType()->isEqual(conformingType));
    return conformance->getAssociatedConformance(assocType, protocol);
  }

  // Otherwise, apply the substitution {self -> conformingType}
  // to the abstract conformance requirement laid upon the dependent type
  // by the protocol.
  auto subMap =
    SubstitutionMap::getProtocolSubstitutions(getRequirement(),
                                              conformingType, *this);
  auto abstractConf = ProtocolConformanceRef(protocol);
  return abstractConf.subst(assocType, subMap);
}

/// Check of all types used by the conformance are canonical.
bool ProtocolConformanceRef::isCanonical() const {
  if (isAbstract() || isInvalid())
    return true;
  if (isPack())
    return getPack()->isCanonical();
  return getConcrete()->isCanonical();

}

ProtocolConformanceRef
ProtocolConformanceRef::getCanonicalConformanceRef() const {
  if (isAbstract() || isInvalid())
    return *this;
  if (isPack())
    return ProtocolConformanceRef(getPack()->getCanonicalConformance());
  return ProtocolConformanceRef(getConcrete()->getCanonicalConformance());
}

bool ProtocolConformanceRef::hasUnavailableConformance() const {
  if (isInvalid() || isAbstract())
    return false;

  if (isPack()) {
    for (auto conformance : getPack()->getPatternConformances()) {
      if (conformance.hasUnavailableConformance())
        return true;
    }

    return false;
  }

  // Check whether this conformance is on an unavailable extension.
  auto concrete = getConcrete();
  auto ext = dyn_cast<ExtensionDecl>(concrete->getDeclContext());
  if (ext && AvailableAttr::isUnavailable(ext))
    return true;

  // Check the conformances in the substitution map.
  auto subMap = concrete->getSubstitutionMap();
  for (auto subConformance : subMap.getConformances()) {
    if (subConformance.hasUnavailableConformance())
      return true;
  }

  return false;
}

bool ProtocolConformanceRef::hasMissingConformance(ModuleDecl *module) const {
  return forEachMissingConformance(module,
      [](BuiltinProtocolConformance *builtin) {
        return true;
      });
}

bool ProtocolConformanceRef::forEachMissingConformance(
    ModuleDecl *module,
    llvm::function_ref<bool(BuiltinProtocolConformance *missing)> fn) const {
  if (isInvalid() || isAbstract())
    return false;

  if (isPack()) {
    for (auto conformance : getPack()->getPatternConformances()) {
      if (conformance.forEachMissingConformance(module, fn))
        return true;
    }

    return false;
  }

  // Is this a missing conformance?
  ProtocolConformance *concreteConf = getConcrete();
  RootProtocolConformance *rootConf = concreteConf->getRootConformance();
  if (auto builtinConformance = dyn_cast<BuiltinProtocolConformance>(rootConf)){
    if (builtinConformance->isMissing() && fn(builtinConformance))
      return true;
  }

  // Check conformances that are part of this conformance.
  auto subMap = concreteConf->getSubstitutionMap();
  for (auto conformance : subMap.getConformances()) {
    if (conformance.forEachMissingConformance(module, fn))
      return true;
  }

  return false;
}

void swift::simple_display(llvm::raw_ostream &out, ProtocolConformanceRef conformanceRef) {
  if (conformanceRef.isAbstract()) {
    simple_display(out, conformanceRef.getAbstract());
  } else if (conformanceRef.isConcrete()) {
    simple_display(out, conformanceRef.getConcrete());
  } else if (conformanceRef.isPack()) {
    simple_display(out, conformanceRef.getPack());
  }
}

SourceLoc swift::extractNearestSourceLoc(const ProtocolConformanceRef conformanceRef) {
  if (conformanceRef.isAbstract()) {
    return extractNearestSourceLoc(conformanceRef.getAbstract());
  } else if (conformanceRef.isConcrete()) {
    return extractNearestSourceLoc(conformanceRef.getConcrete());
  }
  return SourceLoc();
}
