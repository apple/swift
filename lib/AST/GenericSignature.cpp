//===--- GenericSignature.cpp - Generic Signature AST ---------------------===//
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
// This file implements the GenericSignature class.
//
//===----------------------------------------------------------------------===//

#include "GenericSignatureBuilderImpl.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/GenericSignatureBuilder.h"
#include "swift/AST/Decl.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/Module.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/Types.h"
#include "swift/Basic/STLExtras.h"
#include <functional>

using namespace swift;

void ConformanceAccessPath::print(raw_ostream &out) const {
  llvm::interleave(
      begin(), end(),
      [&](const Entry &entry) {
        entry.first.print(out);
        out << ": " << entry.second->getName();
      },
      [&] { out << " -> "; });
}

void ConformanceAccessPath::dump() const {
  print(llvm::errs());
  llvm::errs() << "\n";
}

GenericSignatureImpl::GenericSignatureImpl(
    TypeArrayView<GenericTypeParamType> params,
    ArrayRef<Requirement> requirements, bool isKnownCanonical)
    : NumGenericParams(params.size()), NumRequirements(requirements.size()),
      CanonicalSignatureOrASTContext() {
  std::uninitialized_copy(params.begin(), params.end(),
                          getTrailingObjects<Type>());
  std::uninitialized_copy(requirements.begin(), requirements.end(),
                          getTrailingObjects<Requirement>());

#ifndef NDEBUG
  // Make sure generic parameters are in the right order, and
  // none are missing.
  unsigned depth = 0;
  unsigned count = 0;
  for (auto param : params) {
    if (param->getDepth() != depth) {
      assert(param->getDepth() > depth && "Generic parameter depth mismatch");
      depth = param->getDepth();
      count = 0;
    }
    assert(param->getIndex() == count && "Generic parameter index mismatch");
    ++count;
  }
#endif

  if (isKnownCanonical)
    CanonicalSignatureOrASTContext =
        &GenericSignature::getASTContext(params, requirements);
}

TypeArrayView<GenericTypeParamType>
GenericSignatureImpl::getInnermostGenericParams() const {
  const auto params = getGenericParams();

  const unsigned maxDepth = params.back()->getDepth();
  if (params.front()->getDepth() == maxDepth)
    return params;

  // There is a depth change. Count the number of elements
  // to slice off the front.
  unsigned sliceCount = params.size() - 1;
  while (true) {
    if (params[sliceCount - 1]->getDepth() != maxDepth)
      break;
    --sliceCount;
  }

  return params.slice(sliceCount);
}

void GenericSignatureImpl::forEachParam(
    llvm::function_ref<void(GenericTypeParamType *, bool)> callback) const {
  // Figure out which generic parameters are concrete or same-typed to another
  // type parameter.
  auto genericParams = getGenericParams();
  auto genericParamsAreCanonical =
    SmallVector<bool, 4>(genericParams.size(), true);

  for (auto req : getRequirements()) {
    if (req.getKind() != RequirementKind::SameType) continue;

    GenericTypeParamType *gp;
    if (auto secondGP = req.getSecondType()->getAs<GenericTypeParamType>()) {
      // If two generic parameters are same-typed, then the right-hand one
      // is non-canonical.
      assert(req.getFirstType()->is<GenericTypeParamType>());
      gp = secondGP;
    } else {
      // Otherwise, the right-hand side is an associated type or concrete type,
      // and the left-hand one is non-canonical.
      gp = req.getFirstType()->getAs<GenericTypeParamType>();
      if (!gp) continue;

      // If an associated type is same-typed, it doesn't constrain the generic
      // parameter itself. That is, if T == U.Foo, then T is canonical, whereas
      // U.Foo is not.
      if (req.getSecondType()->isTypeParameter()) continue;
    }

    unsigned index = GenericParamKey(gp).findIndexIn(genericParams);
    genericParamsAreCanonical[index] = false;
  }

  // Call the callback with each parameter and the result of the above analysis.
  for (auto index : indices(genericParams))
    callback(genericParams[index], genericParamsAreCanonical[index]);
}

bool GenericSignatureImpl::areAllParamsConcrete() const {
  unsigned numConcreteGenericParams = 0;
  for (const auto &req : getRequirements()) {
    if (req.getKind() != RequirementKind::SameType) continue;
    if (!req.getFirstType()->is<GenericTypeParamType>()) continue;
    if (req.getSecondType()->isTypeParameter()) continue;

    ++numConcreteGenericParams;
  }

  return numConcreteGenericParams == getGenericParams().size();
}

ASTContext &GenericSignature::getASTContext(
                                    TypeArrayView<GenericTypeParamType> params,
                                    ArrayRef<swift::Requirement> requirements) {
  // The params and requirements cannot both be empty.
  if (!params.empty())
    return params.front()->getASTContext();
  else
    return requirements.front().getFirstType()->getASTContext();
}

GenericSignatureBuilder *
GenericSignatureImpl::getGenericSignatureBuilder() const {
  // The generic signature builder is associated with the canonical signature.
  if (!isCanonical())
    return getCanonicalSignature()->getGenericSignatureBuilder();

  // generic signature builders are stored on the ASTContext.
  return getASTContext().getOrCreateGenericSignatureBuilder(
                                             CanGenericSignature(this));
}

bool GenericSignatureImpl::isEqual(GenericSignature Other) const {
  return getCanonicalSignature() == Other.getCanonicalSignature();
}

bool GenericSignatureImpl::isCanonical() const {
  if (CanonicalSignatureOrASTContext.is<ASTContext *>())
    return true;
  return getCanonicalSignature().getPointer() == this;
}

#ifndef NDEBUG
/// Determine the canonical ordering of requirements.
static unsigned getRequirementKindOrder(RequirementKind kind) {
  switch (kind) {
  case RequirementKind::Conformance: return 2;
  case RequirementKind::Superclass: return 0;
  case RequirementKind::SameType: return 3;
  case RequirementKind::Layout: return 1;
  }
  llvm_unreachable("unhandled kind");
}
#endif

CanGenericSignature
CanGenericSignature::getCanonical(TypeArrayView<GenericTypeParamType> params,
                                  ArrayRef<Requirement> requirements,
                                  bool skipValidation) {
  // Canonicalize the parameters and requirements.
  SmallVector<GenericTypeParamType*, 8> canonicalParams;
  canonicalParams.reserve(params.size());
  for (auto param : params) {
    canonicalParams.push_back(cast<GenericTypeParamType>(param->getCanonicalType()));
  }

  SmallVector<Requirement, 8> canonicalRequirements;
  canonicalRequirements.reserve(requirements.size());
  for (auto &reqt : requirements)
    canonicalRequirements.push_back(reqt.getCanonical());

  (void)skipValidation;
  auto canSig = get(canonicalParams, canonicalRequirements,
                    /*isKnownCanonical=*/true);

#ifndef NDEBUG
  if (skipValidation)
    return CanGenericSignature(canSig);

  PrettyStackTraceGenericSignature debugStack("canonicalizing", canSig);

  // Check that the signature is canonical.
  for (unsigned idx : indices(canonicalRequirements)) {
    debugStack.setRequirement(idx);

    const auto &reqt = canonicalRequirements[idx];

    // Left-hand side must be canonical in its context.
    // Check canonicalization of requirement itself.
    switch (reqt.getKind()) {
    case RequirementKind::Superclass:
      assert(canSig->isCanonicalTypeInContext(reqt.getFirstType()) &&
             "Left-hand side is not canonical");
      assert(canSig->isCanonicalTypeInContext(reqt.getSecondType()) &&
             "Superclass type isn't canonical in its own context");
      break;

    case RequirementKind::Layout:
      assert(canSig->isCanonicalTypeInContext(reqt.getFirstType()) &&
             "Left-hand side is not canonical");
      break;

    case RequirementKind::SameType: {
      auto firstType = reqt.getFirstType();
      auto secondType = reqt.getSecondType();

      if (reqt.getSecondType()->isTypeParameter()) {
        assert(compareDependentTypes(firstType, secondType) < 0 &&
               "Out-of-order type parameters in same-type constraint");
      } else {
        assert(canSig->isCanonicalTypeInContext(secondType) &&
               "Concrete same-type isn't canonical in its own context");
      }
      break;
    }

    case RequirementKind::Conformance:
      assert(reqt.getFirstType()->isTypeParameter() &&
             "Left-hand side must be a type parameter");
      assert(isa<ProtocolType>(reqt.getSecondType().getPointer()) &&
             "Right-hand side of conformance isn't a protocol type");
      break;
    }

    // From here on, we're only interested in requirements beyond the first.
    if (idx == 0) continue;

    // Make sure that the left-hand sides are in nondecreasing order.
    const auto &prevReqt = canonicalRequirements[idx-1];
    int compareLHS =
      compareDependentTypes(prevReqt.getFirstType(), reqt.getFirstType());
    assert(compareLHS <= 0 && "Out-of-order left-hand sides");

    // If we have two same-type requirements where the left-hand sides differ
    // but fall into the same equivalence class, we can check the form.
    if (compareLHS < 0 && reqt.getKind() == RequirementKind::SameType &&
        prevReqt.getKind() == RequirementKind::SameType &&
        canSig->areSameTypeParameterInContext(prevReqt.getFirstType(),
                                              reqt.getFirstType())) {
      // If it's a it's a type parameter, make sure the equivalence class is
      // wired together sanely.
      if (prevReqt.getSecondType()->isTypeParameter()) {
        assert(prevReqt.getSecondType()->isEqual(reqt.getFirstType()) &&
               "same-type constraints within an equiv. class are out-of-order");
      } else {
        // Otherwise, the concrete types must match up.
        assert(prevReqt.getSecondType()->isEqual(reqt.getSecondType()) &&
               "inconsistent concrete same-type constraints in equiv. class");
      }
    }

    // From here on, we only care about cases where the previous and current
    // requirements have the same left-hand side.
    if (compareLHS != 0) continue;

    // Check ordering of requirement kinds.
    assert((getRequirementKindOrder(prevReqt.getKind()) <=
            getRequirementKindOrder(reqt.getKind())) &&
           "Requirements for a given kind are out-of-order");

    // From here on, we only care about the same requirement kind.
    if (prevReqt.getKind() != reqt.getKind()) continue;

    assert(reqt.getKind() == RequirementKind::Conformance &&
           "Only conformance requirements can have multiples");

    auto prevProto =
      prevReqt.getSecondType()->castTo<ProtocolType>()->getDecl();
    auto proto = reqt.getSecondType()->castTo<ProtocolType>()->getDecl();
    assert(TypeDecl::compare(prevProto, proto) < 0 &&
           "Out-of-order conformance requirements");
  }
#endif

  return CanGenericSignature(canSig);
}

CanGenericSignature GenericSignature::getCanonicalSignature() const {
  // If the underlying pointer is null, return `CanGenericSignature()`.
  if (isNull())
    return CanGenericSignature();
  // Otherwise, return the canonical signature of the underlying pointer.
  return getPointer()->getCanonicalSignature();
}

CanGenericSignature GenericSignatureImpl::getCanonicalSignature() const {
  // If we haven't computed the canonical signature yet, do so now.
  if (CanonicalSignatureOrASTContext.isNull()) {
    // Compute the canonical signature.
    auto canSig = CanGenericSignature::getCanonical(getGenericParams(),
                                                    getRequirements());

    // Record either the canonical signature or an indication that
    // this is the canonical signature.
    if (canSig.getPointer() != this)
      CanonicalSignatureOrASTContext = canSig.getPointer();
    else
      CanonicalSignatureOrASTContext = &getGenericParams()[0]->getASTContext();

    // Return the canonical signature.
    return canSig;
  }

  // A stored ASTContext indicates that this is the canonical
  // signature.
  if (CanonicalSignatureOrASTContext.is<ASTContext *>())
    return CanGenericSignature(this);

  // Otherwise, return the stored canonical signature.
  return CanGenericSignature(
      CanonicalSignatureOrASTContext.get<const GenericSignatureImpl *>());
}

GenericEnvironment *GenericSignatureImpl::getGenericEnvironment() const {
  if (GenericEnv == nullptr) {
    auto *builder = getGenericSignatureBuilder();
    const auto impl = const_cast<GenericSignatureImpl *>(this);
    impl->GenericEnv = GenericEnvironment::getIncomplete(this, builder);
  }

  return GenericEnv;
}

ASTContext &GenericSignatureImpl::getASTContext() const {
  // Canonical signatures store the ASTContext directly.
  if (auto ctx = CanonicalSignatureOrASTContext.dyn_cast<ASTContext *>())
    return *ctx;

  // For everything else, just get it from the generic parameter.
  return GenericSignature::getASTContext(getGenericParams(), getRequirements());
}

ProtocolConformanceRef
GenericSignatureImpl::lookupConformance(CanType type,
                                        ProtocolDecl *proto) const {
  // FIXME: Actually implement this properly.
  auto *M = proto->getParentModule();

  if (type->isTypeParameter())
    return ProtocolConformanceRef(proto);

  return M->lookupConformance(type, proto);
}

bool GenericSignatureImpl::requiresClass(Type type) const {
  assert(type->isTypeParameter() &&
         "Only type parameters can have superclass requirements");

  auto &builder = *getGenericSignatureBuilder();
  auto equivClass =
    builder.resolveEquivalenceClass(
                                  type,
                                  ArchetypeResolutionKind::CompleteWellFormed);
  if (!equivClass) return false;

  // If this type was mapped to a concrete type, then there is no
  // requirement.
  if (equivClass->concreteType) return false;

  // If there is a layout constraint, it might be a class.
  if (equivClass->layout && equivClass->layout->isClass()) return true;

  return false;
}

/// Determine the superclass bound on the given dependent type.
Type GenericSignatureImpl::getSuperclassBound(Type type) const {
  assert(type->isTypeParameter() &&
         "Only type parameters can have superclass requirements");

  auto &builder = *getGenericSignatureBuilder();
  auto equivClass =
  builder.resolveEquivalenceClass(
                                type,
                                ArchetypeResolutionKind::CompleteWellFormed);
  if (!equivClass) return nullptr;

  // If this type was mapped to a concrete type, then there is no
  // requirement.
  if (equivClass->concreteType) return nullptr;

  // Retrieve the superclass bound.
  return equivClass->superclass;
}

/// Determine the set of protocols to which the given type parameter is
/// required to conform.
GenericSignature::RequiredProtocols
GenericSignatureImpl::getRequiredProtocols(Type type) const {
  assert(type->isTypeParameter() && "Expected a type parameter");

  auto &builder = *getGenericSignatureBuilder();
  auto equivClass =
    builder.resolveEquivalenceClass(
                                  type,
                                  ArchetypeResolutionKind::CompleteWellFormed);
  if (!equivClass) return { };

  // If this type parameter was mapped to a concrete type, then there
  // are no requirements.
  if (equivClass->concreteType) return { };

  // Retrieve the protocols to which this type conforms.
  GenericSignature::RequiredProtocols result;
  for (const auto &conforms : equivClass->conformsTo)
    result.push_back(conforms.first);

  // Canonicalize the resulting set of protocols.
  ProtocolType::canonicalizeProtocols(result);

  return result;
}

bool GenericSignatureImpl::requiresProtocol(Type type,
                                            ProtocolDecl *proto) const {
  assert(type->isTypeParameter() && "Expected a type parameter");

  auto &builder = *getGenericSignatureBuilder();
  auto equivClass =
    builder.resolveEquivalenceClass(
                                  type,
                                  ArchetypeResolutionKind::CompleteWellFormed);
  if (!equivClass) return false;

  // FIXME: Optionally deal with concrete conformances here
  // or have a separate method do that additionally?
  //
  // If this type parameter was mapped to a concrete type, then there
  // are no requirements.
  if (equivClass->concreteType) return false;

  // Check whether the representative conforms to this protocol.
  return equivClass->conformsTo.count(proto) > 0;
}

/// Determine whether the given dependent type is equal to a concrete type.
bool GenericSignatureImpl::isConcreteType(Type type) const {
  return bool(getConcreteType(type));
}

/// Return the concrete type that the given type parameter is constrained to,
/// or the null Type if it is not the subject of a concrete same-type
/// constraint.
Type GenericSignatureImpl::getConcreteType(Type type) const {
  assert(type->isTypeParameter() && "Expected a type parameter");

  auto &builder = *getGenericSignatureBuilder();
  auto equivClass =
    builder.resolveEquivalenceClass(
                                  type,
                                  ArchetypeResolutionKind::CompleteWellFormed);
  if (!equivClass) return Type();

  return equivClass->concreteType;
}

LayoutConstraint GenericSignatureImpl::getLayoutConstraint(Type type) const {
  assert(type->isTypeParameter() &&
         "Only type parameters can have layout constraints");

  auto &builder = *getGenericSignatureBuilder();
  auto equivClass =
    builder.resolveEquivalenceClass(
                                  type,
                                  ArchetypeResolutionKind::CompleteWellFormed);
  if (!equivClass) return LayoutConstraint();

  return equivClass->layout;
}

bool GenericSignatureImpl::areSameTypeParameterInContext(Type type1,
                                                         Type type2) const {
  assert(type1->isTypeParameter());
  assert(type2->isTypeParameter());

  if (type1.getPointer() == type2.getPointer())
    return true;

  auto &builder = *getGenericSignatureBuilder();
  auto equivClass1 =
    builder.resolveEquivalenceClass(
                             type1,
                             ArchetypeResolutionKind::CompleteWellFormed);
  assert(equivClass1 && "not a valid dependent type of this signature?");

  auto equivClass2 =
    builder.resolveEquivalenceClass(
                             type2,
                             ArchetypeResolutionKind::CompleteWellFormed);
  assert(equivClass2 && "not a valid dependent type of this signature?");

  return equivClass1 == equivClass2;
}

bool GenericSignatureImpl::isRequirementSatisfied(
    Requirement requirement) const {
  auto GSB = getGenericSignatureBuilder();

  auto firstType = requirement.getFirstType();
  auto canFirstType = getCanonicalTypeInContext(firstType);

  switch (requirement.getKind()) {
  case RequirementKind::Conformance: {
    auto protocolType = requirement.getSecondType()->castTo<ProtocolType>();
    auto protocol = protocolType->getDecl();

    if (canFirstType->isTypeParameter())
      return requiresProtocol(canFirstType, protocol);
    else
      return (bool)GSB->lookupConformance(/*dependentType=*/CanType(),
                                          canFirstType, protocol);
  }

  case RequirementKind::SameType: {
    auto canSecondType = getCanonicalTypeInContext(requirement.getSecondType());
    return canFirstType->isEqual(canSecondType);
  }

  case RequirementKind::Superclass: {
    auto requiredSuperclass =
        getCanonicalTypeInContext(requirement.getSecondType());

    // The requirement could be in terms of type parameters like a user-written
    // requirement, but it could also be in terms of concrete types if it has
    // been substituted/otherwise 'resolved', so we need to handle both.
    auto baseType = canFirstType;
    if (baseType->isTypeParameter()) {
      auto directSuperclass = getSuperclassBound(baseType);
      if (!directSuperclass)
        return false;

      baseType = getCanonicalTypeInContext(directSuperclass);
    }

    return requiredSuperclass->isExactSuperclassOf(baseType);
  }

  case RequirementKind::Layout: {
    auto requiredLayout = requirement.getLayoutConstraint();

    if (canFirstType->isTypeParameter()) {
      if (auto layout = getLayoutConstraint(canFirstType))
        return static_cast<bool>(layout.merge(requiredLayout));

      return false;
    }

    // The requirement is on a concrete type, so it's either globally correct
    // or globally incorrect, independent of this generic context. The latter
    // case should be diagnosed elsewhere, so let's assume it's correct.
    return true;
  }
  }
  llvm_unreachable("unhandled kind");
}

SmallVector<Requirement, 4> GenericSignatureImpl::requirementsNotSatisfiedBy(
                                            GenericSignature otherSig) const {
  SmallVector<Requirement, 4> result;

  // If the signatures match by pointer, all requirements are satisfied.
  if (otherSig.getPointer() == this) return result;

  // If there is no other signature, no requirements are satisfied.
  if (!otherSig){
    const auto reqs = getRequirements();
    result.append(reqs.begin(), reqs.end());
    return result;
  }

  // If the canonical signatures are equal, all requirements are satisfied.
  if (getCanonicalSignature() == otherSig->getCanonicalSignature())
    return result;

  // Find the requirements that aren't satisfied.
  for (const auto &req : getRequirements()) {
    if (!otherSig->isRequirementSatisfied(req))
      result.push_back(req);
  }

  return result;
}

bool GenericSignatureImpl::isCanonicalTypeInContext(Type type) const {
  // If the type isn't independently canonical, it's certainly not canonical
  // in this context.
  if (!type->isCanonical())
    return false;

  // All the contextual canonicality rules apply to type parameters, so if the
  // type doesn't involve any type parameters, it's already canonical.
  if (!type->hasTypeParameter())
    return true;

  auto &builder = *getGenericSignatureBuilder();
  return isCanonicalTypeInContext(type, builder);
}

bool GenericSignatureImpl::isCanonicalTypeInContext(
    Type type, GenericSignatureBuilder &builder) const {
  // If the type isn't independently canonical, it's certainly not canonical
  // in this context.
  if (!type->isCanonical())
    return false;

  // All the contextual canonicality rules apply to type parameters, so if the
  // type doesn't involve any type parameters, it's already canonical.
  if (!type->hasTypeParameter())
    return true;

  // Look for non-canonical type parameters.
  return !type.findIf([&](Type component) -> bool {
    if (!component->isTypeParameter()) return false;

    auto equivClass =
      builder.resolveEquivalenceClass(
                               Type(component),
                               ArchetypeResolutionKind::CompleteWellFormed);
    if (!equivClass) return false;

    return (equivClass->concreteType ||
            !component->isEqual(equivClass->getAnchor(builder,
                                                      getGenericParams())));
  });
}

CanType GenericSignatureImpl::getCanonicalTypeInContext(
    Type type, GenericSignatureBuilder &builder) const {
  type = type->getCanonicalType();

  // All the contextual canonicality rules apply to type parameters, so if the
  // type doesn't involve any type parameters, it's already canonical.
  if (!type->hasTypeParameter())
    return CanType(type);

  // Replace non-canonical type parameters.
  type = type.transformRec([&](TypeBase *component) -> Optional<Type> {
    if (!isa<GenericTypeParamType>(component) &&
        !isa<DependentMemberType>(component))
      return None;

    // Find the equivalence class for this dependent type.
    auto resolved = builder.maybeResolveEquivalenceClass(
                      Type(component),
                      ArchetypeResolutionKind::CompleteWellFormed,
                      /*wantExactPotentialArchetype=*/false);
    if (!resolved) return None;

    if (auto concrete = resolved.getAsConcreteType())
      return getCanonicalTypeInContext(concrete, builder);

    auto equivClass = resolved.getEquivalenceClass(builder);
    if (!equivClass) return None;

    if (equivClass->concreteType) {
      return getCanonicalTypeInContext(equivClass->concreteType, builder);
    }

    return equivClass->getAnchor(builder, getGenericParams());
  });
  
  auto result = type->getCanonicalType();

  assert(isCanonicalTypeInContext(result, builder));
  return result;
}

CanType GenericSignatureImpl::getCanonicalTypeInContext(Type type) const {
  type = type->getCanonicalType();

  // All the contextual canonicality rules apply to type parameters, so if the
  // type doesn't involve any type parameters, it's already canonical.
  if (!type->hasTypeParameter())
    return CanType(type);

  auto &builder = *getGenericSignatureBuilder();
  return getCanonicalTypeInContext(type, builder);
}

ArrayRef<CanTypeWrapper<GenericTypeParamType>>
CanGenericSignature::getGenericParams() const{
  auto params = getPointer()->getGenericParams().getOriginalArray();
  auto base = static_cast<const CanTypeWrapper<GenericTypeParamType>*>(
                                                              params.data());
  return {base, params.size()};
}

/// Remove all of the associated type declarations from the given type
/// parameter, producing \c DependentMemberTypes with names alone.
static Type eraseAssociatedTypes(Type type) {
  if (auto depMemTy = type->getAs<DependentMemberType>())
    return DependentMemberType::get(eraseAssociatedTypes(depMemTy->getBase()),
                                    depMemTy->getName());

  return type;
}

namespace {
  typedef GenericSignatureBuilder::RequirementSource RequirementSource;

  template<typename T>
  using GSBConstraint = GenericSignatureBuilder::Constraint<T>;
} // end anonymous namespace

/// Determine whether there is a conformance of the given
/// subject type to the given protocol within the given set of explicit
/// requirements.
static bool hasConformanceInSignature(ArrayRef<Requirement> requirements,
                                      Type subjectType,
                                      ProtocolDecl *proto) {
  // Make sure this requirement exists in the requirement signature.
  for (const auto &req: requirements) {
    if (req.getKind() == RequirementKind::Conformance &&
        req.getFirstType()->isEqual(subjectType) &&
        req.getSecondType()->castTo<ProtocolType>()->getDecl()
          == proto) {
      return true;
    }
  }

  return false;
}

/// Check whether the given requirement source has any non-canonical protocol
/// requirements in it.
static bool hasNonCanonicalSelfProtocolRequirement(
                                          const RequirementSource *source,
                                          ProtocolDecl *conformingProto) {
  for (; source; source = source->parent) {
    // Only look at protocol requirements.
    if (!source->isProtocolRequirement())
      continue;

    // If we don't already have a requirement signature for this protocol,
    // build one now.
    auto inProto = source->getProtocolDecl();

    // Check whether the given requirement is in the requirement signature.
    if (!source->usesRequirementSignature &&
        !hasConformanceInSignature(inProto->getRequirementSignature(),
                                   source->getStoredType(), conformingProto))
      return true;

    // Update the conforming protocol for the rest of the search.
    conformingProto = inProto;
  }

  return false;
}

/// Retrieve the best requirement source from the list
static const RequirementSource *
getBestRequirementSource(GenericSignatureBuilder &builder,
                         ArrayRef<GSBConstraint<ProtocolDecl *>> constraints) {
  const RequirementSource *bestSource = nullptr;
  bool bestIsNonCanonical = false;

  auto isBetter = [&](const RequirementSource *source, bool isNonCanonical) {
    if (!bestSource) return true;

    if (bestIsNonCanonical != isNonCanonical)
      return bestIsNonCanonical;

    return bestSource->compare(source) > 0;
  };

  for (const auto &constraint : constraints) {
    auto source = constraint.source;

    // Skip self-recursive sources.
    bool derivedViaConcrete = false;
    if (source->getMinimalConformanceSource(
                                        builder,
                                        constraint.getSubjectDependentType({ }),
                                        constraint.value,
                                        derivedViaConcrete)
          != source)
      continue;

    // If there is a non-canonical protocol requirement next to the root,
    // skip this requirement source.
    bool isNonCanonical =
      hasNonCanonicalSelfProtocolRequirement(source, constraint.value);

    if (isBetter(source, isNonCanonical)) {
      bestSource = source;
      bestIsNonCanonical = isNonCanonical;
      continue;
    }
  }

  assert(bestSource && "All sources were self-recursive?");
  return bestSource;
}

void GenericSignatureImpl::buildConformanceAccessPath(
    SmallVectorImpl<ConformanceAccessPath::Entry> &path,
    ArrayRef<Requirement> reqs, const void *opaqueSource,
    ProtocolDecl *conformingProto, Type rootType,
    ProtocolDecl *requirementSignatureProto) const {
  auto *source = reinterpret_cast<const RequirementSource *>(opaqueSource);
  // Each protocol requirement is a step along the path.
  if (source->isProtocolRequirement()) {
    // If we're expanding for a protocol that had no requirement signature
    // and have hit the penultimate step, this is the last step
    // that would occur in the requirement signature.
    Optional<GenericSignatureBuilder> replacementBuilder;
    if (!source->parent->parent && requirementSignatureProto) {
      // If we have a requirement signature now, we're done.
      if (source->usesRequirementSignature) {
        Type subjectType = source->getStoredType()->getCanonicalType();
        path.push_back({subjectType, conformingProto});
        return;
      }

      // The generic signature builder we're using for this protocol
      // wasn't built from its own requirement signature, so we can't
      // trust it, build a new generic signature builder.
      // FIXME: It would be better if we could replace the canonical generic
      // signature builder with the rebuilt one.

      replacementBuilder.emplace(getASTContext());
      replacementBuilder->addGenericSignature(
                          requirementSignatureProto->getGenericSignature());
      replacementBuilder->processDelayedRequirements();
    }

    // Follow the rest of the path to derive the conformance into which
    // this particular protocol requirement step would look.
    auto inProtocol = source->getProtocolDecl();
    buildConformanceAccessPath(path, reqs, source->parent, inProtocol, rootType,
                               requirementSignatureProto);
    assert(path.back().second == inProtocol &&
           "path produces incorrect conformance");

    // If this step was computed via the requirement signature, add it
    // directly.
    if (source->usesRequirementSignature) {
      // Add this step along the path, which involves looking for the
      // conformance we want (\c conformingProto) within the protocol
      // described by this source.

      // Canonicalize the subject type within the protocol's generic
      // signature.
      Type subjectType = source->getStoredType();
      subjectType = inProtocol->getGenericSignature()
        ->getCanonicalTypeInContext(subjectType);

      assert(hasConformanceInSignature(inProtocol->getRequirementSignature(),
                                       subjectType, conformingProto) &&
             "missing explicit conformance in requirement signature");

      // Record this step.
      path.push_back({subjectType, conformingProto});
      return;
    }

    // Get the generic signature builder for the protocol.
    // Get a generic signature for the protocol's signature.
    auto inProtoSig = inProtocol->getGenericSignature();
    auto &inProtoSigBuilder =
        replacementBuilder ? *replacementBuilder
                           : *inProtoSig->getGenericSignatureBuilder();

    // Retrieve the stored type, but erase all of the specific associated
    // type declarations; we don't want any details of the enclosing context
    // to sneak in here.
    Type storedType = eraseAssociatedTypes(source->getStoredType());

    // Dig out the potential archetype for this stored type.
    auto equivClass =
      inProtoSigBuilder.resolveEquivalenceClass(
                               storedType,
                               ArchetypeResolutionKind::CompleteWellFormed);

    // Find the conformance of this potential archetype to the protocol in
    // question.
    auto conforms = equivClass->conformsTo.find(conformingProto);
    assert(conforms != equivClass->conformsTo.end());

    // Compute the root type, canonicalizing it w.r.t. the protocol context.
    auto conformsSource = getBestRequirementSource(inProtoSigBuilder,
                                                   conforms->second);
    assert(conformsSource != source || !requirementSignatureProto);
    Type localRootType = conformsSource->getRootType();
    localRootType = inProtoSig->getCanonicalTypeInContext(localRootType);

    // Build the path according to the requirement signature.
    buildConformanceAccessPath(path, inProtocol->getRequirementSignature(),
                               conformsSource, conformingProto, localRootType,
                               inProtocol);

    // We're done.
    return;
  }

  // If we have a superclass or concrete requirement, the conformance
  // we need is stored in it.
  if (source->kind == RequirementSource::Superclass ||
      source->kind == RequirementSource::Concrete) {
    auto conformance = source->getProtocolConformance();
    (void)conformance;
    assert(conformance.getRequirement() == conformingProto);
    path.push_back({source->getAffectedType(), conformingProto});
    return;
  }

  // If we still have a parent, keep going.
  if (source->parent) {
    buildConformanceAccessPath(path, reqs, source->parent, conformingProto,
                               rootType, requirementSignatureProto);
    return;
  }

  // We are at an explicit or inferred requirement.
  assert(source->kind == RequirementSource::Explicit ||
         source->kind == RequirementSource::Inferred);

  // Skip trivial path elements. These occur when querying a requirement
  // signature.
  if (!path.empty() && conformingProto == path.back().second &&
      rootType->isEqual(conformingProto->getSelfInterfaceType()))
    return;

  assert(hasConformanceInSignature(reqs, rootType, conformingProto) &&
         "missing explicit conformance in signature");

  // Add the root of the path, which starts at this explicit requirement.
  path.push_back({rootType, conformingProto});
}

ConformanceAccessPath
GenericSignatureImpl::getConformanceAccessPath(Type type,
                                               ProtocolDecl *protocol) const {
  assert(type->isTypeParameter() && "not a type parameter");

  // Resolve this type to a potential archetype.
  auto &builder = *getGenericSignatureBuilder();
  auto equivClass =
    builder.resolveEquivalenceClass(
                                  type,
                                  ArchetypeResolutionKind::CompleteWellFormed);

  auto cached = equivClass->conformanceAccessPathCache.find(protocol);
  if (cached != equivClass->conformanceAccessPathCache.end())
    return cached->second;

  // Dig out the conformance of this type to the given protocol, because we
  // want its requirement source.
  auto conforms = equivClass->conformsTo.find(protocol);
  assert(conforms != equivClass->conformsTo.end());

  // Canonicalize the root type.
  auto source = getBestRequirementSource(builder, conforms->second);
  Type rootType = source->getRootType()->getCanonicalType(this);

  // Build the path.
  SmallVector<ConformanceAccessPath::Entry, 2> path;
  buildConformanceAccessPath(path, getRequirements(), source, protocol,
                             rootType, nullptr);

  // Return the path; we're done!
  ConformanceAccessPath result(getASTContext().AllocateCopy(path));
  equivClass->conformanceAccessPathCache.insert({protocol, result});
  return result;
}

unsigned GenericParamKey::findIndexIn(
                      TypeArrayView<GenericTypeParamType> genericParams) const {
  // For depth 0, we have random access. We perform the extra checking so that
  // we can return
  if (Depth == 0 && Index < genericParams.size() &&
      genericParams[Index] == *this)
    return Index;

  // At other depths, perform a binary search.
  unsigned result =
      std::lower_bound(genericParams.begin(), genericParams.end(), *this,
                       Ordering())
        - genericParams.begin();
  if (result < genericParams.size() && genericParams[result] == *this)
    return result;

  // We didn't find the parameter we were looking for.
  return genericParams.size();
}

SubstitutionMap GenericSignatureImpl::getIdentitySubstitutionMap() const {
  return SubstitutionMap::get(const_cast<GenericSignatureImpl *>(this),
                              [](SubstitutableType *t) -> Type {
                                return Type(cast<GenericTypeParamType>(t));
                              },
                              MakeAbstractConformanceForGenericType());
}

GenericTypeParamType *GenericSignatureImpl::getSugaredType(
    GenericTypeParamType *type) const {
  unsigned ordinal = getGenericParamOrdinal(type);
  return getGenericParams()[ordinal];
}

Type GenericSignatureImpl::getSugaredType(Type type) const {
  if (!type->hasTypeParameter())
    return type;

  return type.transform([this](Type Ty) -> Type {
    if (auto GP = dyn_cast<GenericTypeParamType>(Ty.getPointer())) {
      return Type(getSugaredType(GP));
    }
    return Ty;
  });
}

unsigned GenericSignatureImpl::getGenericParamOrdinal(
    GenericTypeParamType *param) const {
  return GenericParamKey(param).findIndexIn(getGenericParams());
}

bool GenericSignatureImpl::hasTypeVariable() const {
  return GenericSignature::hasTypeVariable(getRequirements());
}

bool GenericSignature::hasTypeVariable(ArrayRef<Requirement> requirements) {
  for (const auto &req : requirements) {
    if (req.getFirstType()->hasTypeVariable())
      return true;

    switch (req.getKind()) {
    case RequirementKind::Layout:
      break;

    case RequirementKind::Conformance:
    case RequirementKind::SameType:
    case RequirementKind::Superclass:
      if (req.getSecondType()->hasTypeVariable())
        return true;
      break;
    }
  }

  return false;
}

void GenericSignature::Profile(llvm::FoldingSetNodeID &id) const {
  return GenericSignature::Profile(id, getPointer()->getGenericParams(),
                                   getPointer()->getRequirements());
}

void GenericSignature::Profile(llvm::FoldingSetNodeID &ID,
                               TypeArrayView<GenericTypeParamType> genericParams,
                               ArrayRef<Requirement> requirements) {
  return GenericSignatureImpl::Profile(ID, genericParams, requirements);
}

void swift::simple_display(raw_ostream &out, GenericSignature sig) {
  if (sig)
    sig->print(out);
  else
    out << "NULL";
}

bool Requirement::isCanonical() const {
  if (getFirstType() && !getFirstType()->isCanonical())
    return false;

  switch (getKind()) {
  case RequirementKind::Conformance:
  case RequirementKind::SameType:
  case RequirementKind::Superclass:
    if (getSecondType() && !getSecondType()->isCanonical())
      return false;
    break;

  case RequirementKind::Layout:
    break;
  }

  return true;
}


/// Get the canonical form of this requirement.
Requirement Requirement::getCanonical() const {
  Type firstType = getFirstType();
  if (firstType)
    firstType = firstType->getCanonicalType();

  switch (getKind()) {
  case RequirementKind::Conformance:
  case RequirementKind::SameType:
  case RequirementKind::Superclass: {
    Type secondType = getSecondType();
    if (secondType)
      secondType = secondType->getCanonicalType();
    return Requirement(getKind(), firstType, secondType);
  }

  case RequirementKind::Layout:
    return Requirement(getKind(), firstType, getLayoutConstraint());
  }
  llvm_unreachable("Unhandled RequirementKind in switch");
}
