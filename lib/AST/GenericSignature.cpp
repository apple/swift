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

#include "swift/AST/GenericSignature.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/Module.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/AST/Types.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Basic/STLExtras.h"
#include "RequirementMachine/RequirementMachine.h"
#include <functional>

using namespace swift;

void ConformancePath::print(raw_ostream &out) const {
  llvm::interleave(
      begin(), end(),
      [&](const Entry &entry) {
        entry.first.print(out);
        out << ": " << entry.second->getName();
      },
      [&] { out << " -> "; });
}

void ConformancePath::dump() const {
  print(llvm::errs());
  llvm::errs() << "\n";
}

GenericSignatureImpl::GenericSignatureImpl(
    ArrayRef<GenericTypeParamType *> params,
    ArrayRef<Requirement> requirements, bool isKnownCanonical)
    : NumGenericParams(params.size()), NumRequirements(requirements.size()),
      CanonicalSignatureOrASTContext() {
  std::uninitialized_copy(params.begin(), params.end(),
                          getTrailingObjects<GenericTypeParamType *>());
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

ArrayRef<GenericTypeParamType *>
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
    GenericTypeParamType *gp;
    switch (req.getKind()) {
    case RequirementKind::SameType: {
      if (auto secondGP = req.getSecondType()->getAs<GenericTypeParamType>()) {
        // If two generic parameters are same-typed, then the right-hand one
        // is non-canonical.
        assert(req.getFirstType()->is<GenericTypeParamType>());
        gp = secondGP;
      } else {
        // Otherwise, the right-hand side is an associated type or concrete
        // type, and the left-hand one is non-canonical.
        gp = req.getFirstType()->getAs<GenericTypeParamType>();
        if (!gp)
          continue;

        // If an associated type is same-typed, it doesn't constrain the generic
        // parameter itself. That is, if T == U.Foo, then T is canonical,
        // whereas U.Foo is not.
        if (req.getSecondType()->isTypeParameter())
          continue;
      }
      break;
    }

    case RequirementKind::Superclass:
    case RequirementKind::Conformance:
    case RequirementKind::Layout:
    case RequirementKind::SameShape:
      continue;
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
    switch (req.getKind()) {
    case RequirementKind::SameType:
      if (!req.getFirstType()->is<GenericTypeParamType>())
        continue;
      if (req.getSecondType()->isTypeParameter())
        continue;

      ++numConcreteGenericParams;
      break;

    case RequirementKind::Conformance:
    case RequirementKind::Superclass:
    case RequirementKind::Layout:
    case RequirementKind::SameShape:
      continue;
    }
  }

  return numConcreteGenericParams == getGenericParams().size();
}

bool GenericSignatureImpl::hasParameterPack() const {
  for (auto *paramTy : getGenericParams()) {
    if (paramTy->isParameterPack())
      return true;
  }

  return false;
}

ASTContext &GenericSignature::getASTContext(
                                    ArrayRef<GenericTypeParamType *> params,
                                    ArrayRef<swift::Requirement> requirements) {
  // The params and requirements cannot both be empty.
  if (!params.empty())
    return params.front()->getASTContext();
  else
    return requirements.front().getFirstType()->getASTContext();
}

/// Retrieve the generic parameters.
ArrayRef<GenericTypeParamType *> GenericSignature::getGenericParams() const {
  return isNull()
      ? ArrayRef<GenericTypeParamType *>()
      : getPointer()->getGenericParams();
}

/// Retrieve the innermost generic parameters.
///
/// Given a generic signature for a nested generic type, produce an
/// array of the generic parameters for the innermost generic type.
ArrayRef<GenericTypeParamType *> GenericSignature::getInnermostGenericParams() const {
  return isNull()
      ? ArrayRef<GenericTypeParamType *>()
      : getPointer()->getInnermostGenericParams();
}

/// Retrieve the requirements.
ArrayRef<Requirement> GenericSignature::getRequirements() const {
  return isNull()
      ? ArrayRef<Requirement>{}
      : getPointer()->getRequirements();
}

rewriting::RequirementMachine *
GenericSignatureImpl::getRequirementMachine() const {
  if (Machine)
    return Machine;

  const_cast<GenericSignatureImpl *>(this)->Machine
      = getASTContext().getRewriteContext().getRequirementMachine(
          getCanonicalSignature());
  return Machine;
}

bool GenericSignatureImpl::isEqual(GenericSignature Other) const {
  return getCanonicalSignature() == Other.getCanonicalSignature();
}

bool GenericSignatureImpl::isCanonical() const {
  if (CanonicalSignatureOrASTContext.is<ASTContext *>())
    return true;
  return getCanonicalSignature().getPointer() == this;
}

CanGenericSignature
CanGenericSignature::getCanonical(ArrayRef<GenericTypeParamType *> params,
                                  ArrayRef<Requirement> requirements) {
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

  auto canSig = get(canonicalParams, canonicalRequirements,
                    /*isKnownCanonical=*/true);

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

GenericEnvironment *GenericSignature::getGenericEnvironment() const {
  if (isNull())
    return nullptr;
  return getPointer()->getGenericEnvironment();
}

GenericEnvironment *GenericSignatureImpl::getGenericEnvironment() const {
  if (GenericEnv == nullptr) {
    const auto impl = const_cast<GenericSignatureImpl *>(this);
    impl->GenericEnv = GenericEnvironment::forPrimary(this);
  }

  return GenericEnv;
}

GenericSignature::LocalRequirements
GenericSignatureImpl::getLocalRequirements(Type depType) const {
  assert(depType->isTypeParameter() && "Expected a type parameter here");

  return getRequirementMachine()->getLocalRequirements(
      depType, getGenericParams());
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

  return M->lookupConformance(type, proto, /*allowMissing=*/true);
}

bool GenericSignatureImpl::requiresClass(Type type) const {
  assert(type->isTypeParameter() &&
         "Only type parameters can have superclass requirements");

  return getRequirementMachine()->requiresClass(type);
}

/// Determine the superclass bound on the given dependent type.
Type GenericSignatureImpl::getSuperclassBound(Type type) const {
  assert(type->isTypeParameter() &&
         "Only type parameters can have superclass requirements");

  return getRequirementMachine()->getSuperclassBound(
      type, getGenericParams());
}

/// Determine the set of protocols to which the given type parameter is
/// required to conform.
GenericSignature::RequiredProtocols
GenericSignatureImpl::getRequiredProtocols(Type type) const {
  assert(type->isTypeParameter() && "Expected a type parameter");

  return getRequirementMachine()->getRequiredProtocols(type);
}

bool GenericSignatureImpl::requiresProtocol(Type type,
                                            ProtocolDecl *proto) const {
  assert(type->isTypeParameter() && "Expected a type parameter");

  return getRequirementMachine()->requiresProtocol(type, proto);
}

/// Determine whether the given dependent type is equal to a concrete type.
bool GenericSignatureImpl::isConcreteType(Type type) const {
  assert(type->isTypeParameter() && "Expected a type parameter");

  return getRequirementMachine()->isConcreteType(type);
}

/// Return the concrete type that the given type parameter is constrained to,
/// or the null Type if it is not the subject of a concrete same-type
/// constraint.
Type GenericSignatureImpl::getConcreteType(Type type) const {
  assert(type->isTypeParameter() && "Expected a type parameter");

  return getRequirementMachine()->getConcreteType(type, getGenericParams());
}

LayoutConstraint GenericSignatureImpl::getLayoutConstraint(Type type) const {
  assert(type->isTypeParameter() &&
         "Only type parameters can have layout constraints");

  return getRequirementMachine()->getLayoutConstraint(type);
}

bool GenericSignatureImpl::areReducedTypeParametersEqual(Type type1,
                                                         Type type2) const {
  assert(type1->isTypeParameter());
  assert(type2->isTypeParameter());

  if (type1.getPointer() == type2.getPointer())
    return true;

  return getRequirementMachine()->areReducedTypeParametersEqual(type1, type2);
}

bool GenericSignatureImpl::isRequirementSatisfied(
    Requirement requirement, bool allowMissing) const {
  if (requirement.getFirstType()->hasTypeParameter()) {
    auto *genericEnv = getGenericEnvironment();

    requirement = requirement.subst(
        [&](SubstitutableType *type) -> Type {
          if (auto *paramType = type->getAs<GenericTypeParamType>())
            return genericEnv->mapTypeIntoContext(paramType);

          return type;
        },
        LookUpConformanceInSignature(this));
  }

  SmallVector<Requirement, 2> subReqs;
  switch (requirement.checkRequirement(subReqs, allowMissing)) {
  case CheckRequirementResult::Success:
    return true;

  case CheckRequirementResult::ConditionalConformance:
    // FIXME: Need to check conditional requirements here.
    return true;

  case CheckRequirementResult::PackRequirement:
    // FIXME
    assert(false && "Refactor this");
    return true;

  case CheckRequirementResult::RequirementFailure:
  case CheckRequirementResult::SubstitutionFailure:
    return false;
  }
}

SmallVector<Requirement, 4>
GenericSignature::requirementsNotSatisfiedBy(GenericSignature otherSig) const {
  // The null generic signature has no requirements, therefore all requirements
  // are satisfied by any signature.
  if (isNull()) {
    return {};
  }
  return getPointer()->requirementsNotSatisfiedBy(otherSig);
}

SmallVector<Requirement, 4> GenericSignatureImpl::requirementsNotSatisfiedBy(
                                            GenericSignature otherSig) const {
  SmallVector<Requirement, 4> result;

  // If the signatures match by pointer, all requirements are satisfied.
  if (otherSig.getPointer() == this) return result;

  // If there is no other signature, no requirements are satisfied.
  if (!otherSig) {
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

bool GenericSignatureImpl::isReducedType(Type type) const {
  // If the type isn't canonical, it's not reduced.
  if (!type->isCanonical())
    return false;

  // A fully concrete canonical type is reduced.
  if (!type->hasTypeParameter())
    return true;

  return getRequirementMachine()->isReducedType(type);
}

CanType GenericSignature::getReducedType(Type type) const {
  // The null generic signature has no requirements so cannot influence the
  // structure of the can type computed here.
  if (isNull()) {
    return type->getCanonicalType();
  }
  return getPointer()->getReducedType(type);
}

GenericSignature GenericSignature::typeErased(ArrayRef<Type> typeErasedParams) const {
  bool changedSignature = false;
  llvm::SmallVector<Requirement, 4> requirementsErased;

  for (auto req : getRequirements()) {
    bool found = std::any_of(typeErasedParams.begin(),
                             typeErasedParams.end(),
                             [&](Type t) {
      auto other = req.getFirstType();
      return t->isEqual(other);
    });
    if (found) {
      requirementsErased.push_back(Requirement(RequirementKind::SameType,
                                               req.getFirstType(),
                                               Ptr->getASTContext().getAnyObjectType()));
    } else {
      requirementsErased.push_back(req);
    }
    changedSignature |= found;
  }

  if (changedSignature) {
    return GenericSignature::get(getGenericParams(),
                                 requirementsErased, false);
  }

  return *this;
}

CanType GenericSignatureImpl::getReducedType(Type type) const {
  type = type->getCanonicalType();

  // A fully concrete type is already reduced.
  if (!type->hasTypeParameter())
    return CanType(type);

  return getRequirementMachine()->getReducedType(
      type, { })->getCanonicalType();
}

bool GenericSignatureImpl::isValidTypeParameter(Type type) const {
  return getRequirementMachine()->isValidTypeParameter(type);
}

ArrayRef<CanTypeWrapper<GenericTypeParamType>>
CanGenericSignature::getGenericParams() const {
  auto params =
      this->GenericSignature::getGenericParams();
  auto base = reinterpret_cast<const CanTypeWrapper<GenericTypeParamType> *>(
                                                              params.data());
  return {base, params.size()};
}

ConformancePath
GenericSignatureImpl::getConformancePath(Type type,
                                         ProtocolDecl *protocol) const {
  return getRequirementMachine()->getConformancePath(type, protocol);
}

TypeDecl *
GenericSignatureImpl::lookupNestedType(Type type, Identifier name) const {
  assert(type->isTypeParameter());

  return getRequirementMachine()->lookupNestedType(type, name);
}

Type
GenericSignatureImpl::getReducedShape(Type type) const {
  return getRequirementMachine()->getReducedShape(type, getGenericParams());
}

bool
GenericSignatureImpl::haveSameShape(Type type1, Type type2) const {
  return getRequirementMachine()->haveSameShape(type1, type2);
}

SmallVector<CanType, 2> GenericSignatureImpl::getShapeClasses() const {
  SmallSetVector<CanType, 2> result;

  forEachParam([&](GenericTypeParamType *gp, bool canonical) {
    if (!canonical || !gp->isParameterPack())
      return;

    result.insert(getReducedShape(gp)->getCanonicalType());
  });

  return result.takeVector();
}

unsigned GenericParamKey::findIndexIn(
                      ArrayRef<GenericTypeParamType *> genericParams) const {
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
      auto param = cast<GenericTypeParamType>(t);
      if (!param->isParameterPack())
        return param;
      return PackType::getSingletonPackExpansion(param);
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

Type GenericSignatureImpl::getNonDependentUpperBounds(Type type) const {
  return getUpperBound(type);
}

Type GenericSignatureImpl::getDependentUpperBounds(Type type) const {
  return getUpperBound(type, /*wantDependentBound=*/true);
}

Type GenericSignatureImpl::getUpperBound(Type type,
                                         bool wantDependentBound) const {
  assert(type->isTypeParameter());

  bool hasExplicitAnyObject = requiresClass(type);

  llvm::SmallVector<Type, 2> types;

  if (Type superclass = getSuperclassBound(type)) {
    do {
      superclass = getReducedType(superclass);
      if (wantDependentBound || !superclass->hasTypeParameter()) {
        break;
      }
    } while ((superclass = superclass->getSuperclass()));

    if (superclass) {
      types.push_back(superclass);
      hasExplicitAnyObject = false;
    }
  }

  for (auto *proto : getRequiredProtocols(type)) {
    if (proto->requiresClass())
      hasExplicitAnyObject = false;

    auto *baseType = proto->getDeclaredInterfaceType()->castTo<ProtocolType>();

    auto primaryAssocTypes = proto->getPrimaryAssociatedTypes();
    if (!primaryAssocTypes.empty()) {
      SmallVector<Type, 2> argTypes;

      // Attempt to recover same-type requirements on primary associated types.
      for (auto *assocType : primaryAssocTypes) {
        // For each primary associated type A of P, compute the reduced type
        // of T.[P]A.
        auto *memberType = DependentMemberType::get(type, assocType);
        auto reducedType = getReducedType(memberType);

        // If the reduced type is at a lower depth than the root generic
        // parameter of T, then it's constrained.
        bool hasOuterGenericParam = false;
        bool hasInnerGenericParam = false;
        reducedType.visit([&](Type t) {
          if (auto *paramTy = t->getAs<GenericTypeParamType>()) {
            unsigned rootDepth = type->getRootGenericParam()->getDepth();
            if (paramTy->getDepth() == rootDepth)
              hasInnerGenericParam = true;
            else {
              assert(paramTy->getDepth() < rootDepth);
              hasOuterGenericParam = true;
            }
          }
        });

        if (hasInnerGenericParam && hasOuterGenericParam) {
          llvm::errs() << "Weird same-type requirements?\n";
          llvm::errs() << "Interface type: " << type << "\n";
          llvm::errs() << "Member type: " << memberType << "\n";
          llvm::errs() << "Reduced member type: " << reducedType << "\n";
          llvm::errs() << GenericSignature(this) << "\n";
          abort();
        }

        if (!hasInnerGenericParam && (wantDependentBound || !hasOuterGenericParam))
          argTypes.push_back(reducedType);
      }

      // If we have constrained all primary associated types, create a
      // parameterized protocol type. During code completion, we might call
      // `getExistentialType` (which calls this method) on a generic parameter
      // that doesn't have all parameters specified, e.g. to get a consise
      // description of the parameter type to the following function.
      //
      // func foo<P: Publisher>(p: P) where P.Failure == Never
      //
      // In that case just add the base type in the default branch below.
      if (argTypes.size() == primaryAssocTypes.size()) {
        types.push_back(ParameterizedProtocolType::get(getASTContext(), baseType, argTypes));
        continue;
      }
    }
    types.push_back(baseType);
  }

  auto constraint = ProtocolCompositionType::get(getASTContext(), types,
                                                 hasExplicitAnyObject);

  if (!constraint->isConstraintType()) {
    assert(constraint->getClassOrBoundGenericClass());
    return constraint;
  }

  return ExistentialType::get(constraint);
}

void GenericSignature::Profile(llvm::FoldingSetNodeID &id) const {
  return GenericSignature::Profile(id, getPointer()->getGenericParams(),
                                     getPointer()->getRequirements());
}

void GenericSignature::Profile(llvm::FoldingSetNodeID &ID,
                               ArrayRef<GenericTypeParamType *> genericParams,
                               ArrayRef<Requirement> requirements) {
  return GenericSignatureImpl::Profile(ID, genericParams, requirements);
}

void swift::simple_display(raw_ostream &out, GenericSignature sig) {
  if (sig)
    sig->print(out);
  else
    out << "NULL";
}

/// Compare two associated types.
int swift::compareAssociatedTypes(AssociatedTypeDecl *assocType1,
                                  AssociatedTypeDecl *assocType2) {
  // - by name.
  if (int result = assocType1->getName().str().compare(
                                              assocType2->getName().str()))
    return result;

  // Prefer an associated type with no overrides (i.e., an anchor) to one
  // that has overrides.
  bool hasOverridden1 = !assocType1->getOverriddenDecls().empty();
  bool hasOverridden2 = !assocType2->getOverriddenDecls().empty();
  if (hasOverridden1 != hasOverridden2)
    return hasOverridden1 ? +1 : -1;

  // - by protocol, so t_n_m.`P.T` < t_n_m.`Q.T` (given P < Q)
  auto proto1 = assocType1->getProtocol();
  auto proto2 = assocType2->getProtocol();
  if (int compareProtocols = TypeDecl::compare(proto1, proto2))
    return compareProtocols;

  // Error case: if we have two associated types with the same name in the
  // same protocol, just tie-break based on source location.
  if (assocType1 != assocType2) {
    auto &ctx = assocType1->getASTContext();
    return ctx.SourceMgr.isBeforeInBuffer(assocType1->getLoc(),
                                          assocType2->getLoc()) ? -1 : +1;
  }

  return 0;
}

/// Canonical ordering for type parameters.
int swift::compareDependentTypes(Type type1, Type type2) {
  // Fast-path check for equality.
  if (type1->isEqual(type2)) return 0;

  // Ordering is as follows:
  // - Generic params
  auto gp1 = type1->getAs<GenericTypeParamType>();
  auto gp2 = type2->getAs<GenericTypeParamType>();
  if (gp1 && gp2)
    return GenericParamKey(gp1) < GenericParamKey(gp2) ? -1 : +1;

  // A generic parameter is always ordered before a nested type.
  if (static_cast<bool>(gp1) != static_cast<bool>(gp2))
    return gp1 ? -1 : +1;

  // - Dependent members
  auto depMemTy1 = type1->castTo<DependentMemberType>();
  auto depMemTy2 = type2->castTo<DependentMemberType>();

  // - by base, so t_0_n.`P.T` < t_1_m.`P.T`
  if (int compareBases =
        compareDependentTypes(depMemTy1->getBase(), depMemTy2->getBase()))
    return compareBases;

  // - by name, so t_n_m.`P.T` < t_n_m.`P.U`
  if (int compareNames = depMemTy1->getName().str().compare(
                                                  depMemTy2->getName().str()))
    return compareNames;

  auto *assocType1 = depMemTy1->getAssocType();
  auto *assocType2 = depMemTy2->getAssocType();
  if (int result = compareAssociatedTypes(assocType1, assocType2))
    return result;

  return 0;
}

#pragma mark Generic signature verification

void GenericSignature::verify() const {
  verify(getRequirements());
}

void GenericSignature::verify(ArrayRef<Requirement> reqts) const {
  auto canSig = getCanonicalSignature();

  PrettyStackTraceGenericSignature debugStack("checking", canSig);

  // We collect conformance requirements to check that they're minimal.
  llvm::SmallDenseMap<CanType, SmallVector<ProtocolDecl *, 2>, 2> conformances;

  // We collect same-type requirements to check that they're minimal.
  llvm::SmallDenseMap<CanType, SmallVector<Type, 2>, 2> sameTypeComponents;

  // Check that the requirements satisfy certain invariants.
  for (unsigned idx : indices(reqts)) {
    const auto &reqt = reqts[idx].getCanonical();

    // Left-hand side must be a canonical type parameter.
    if (reqt.getKind() != RequirementKind::SameType) {
      if (!reqt.getFirstType()->isTypeParameter()) {
        llvm::errs() << "Left-hand side must be a type parameter: ";
        reqt.dump(llvm::errs());
        llvm::errs() << "\n";
        abort();
      }

      if (!canSig->isReducedType(reqt.getFirstType())) {
        llvm::errs() << "Left-hand side is not reduced: ";
        reqt.dump(llvm::errs());
        llvm::errs() << "\n";
        abort();
      }
    }

    // Check canonicalization of requirement itself.
    switch (reqt.getKind()) {
    case RequirementKind::SameShape:
      if (!reqt.getFirstType()->is<GenericTypeParamType>()) {
        llvm::errs() << "Left hand side is not a generic parameter: ";
        reqt.dump(llvm::errs());
        llvm::errs() << "\n";
        abort();
      }

      if (!reqt.getFirstType()->castTo<GenericTypeParamType>()->isParameterPack()) {
        llvm::errs() << "Left hand side is not a parameter pack: ";
        reqt.dump(llvm::errs());
        llvm::errs() << "\n";
        abort();
      }

      if (!reqt.getSecondType()->is<GenericTypeParamType>()) {
        llvm::errs() << "Right hand side is not a generic parameter: ";
        reqt.dump(llvm::errs());
        llvm::errs() << "\n";
        abort();
      }

      if (!reqt.getSecondType()->castTo<GenericTypeParamType>()->isParameterPack()) {
        llvm::errs() << "Right hand side is not a parameter pack: ";
        reqt.dump(llvm::errs());
        llvm::errs() << "\n";
        abort();
      }

      break;
    case RequirementKind::Superclass:
      if (!canSig->isReducedType(reqt.getSecondType())) {
        llvm::errs() << "Right-hand side is not reduced: ";
        reqt.dump(llvm::errs());
        llvm::errs() << "\n";
        abort();
      }
      break;

    case RequirementKind::Layout:
      break;

    case RequirementKind::SameType: {
      auto hasReducedOrConcreteParent = [&](Type type) {
        if (auto *dmt = type->getAs<DependentMemberType>()) {
          return (canSig->isReducedType(dmt->getBase()) ||
                  canSig->isConcreteType(dmt->getBase()));
        }
        return type->is<GenericTypeParamType>();
      };

      auto firstType = reqt.getFirstType();
      auto secondType = reqt.getSecondType();

      auto canType = canSig->getReducedType(firstType);
      auto &component = sameTypeComponents[canType];

      if (!hasReducedOrConcreteParent(firstType)) {
        llvm::errs() << "Left hand side does not have a reduced parent: ";
        reqt.dump(llvm::errs());
        llvm::errs() << "\n";
        abort();
      }

      if (reqt.getSecondType()->isTypeParameter()) {
        if (!hasReducedOrConcreteParent(secondType)) {
          llvm::errs() << "Right hand side does not have a reduced parent: ";
          reqt.dump(llvm::errs());
          llvm::errs() << "\n";
          abort();
        }
        if (compareDependentTypes(firstType, secondType) >= 0) {
          llvm::errs() << "Out-of-order type parameters: ";
          reqt.dump(llvm::errs());
          llvm::errs() << "\n";
          abort();
        }

        if (component.empty()) {
          component.push_back(firstType);
        } else if (!component.back()->isEqual(firstType)) {
          llvm::errs() << "Same-type requirement within an equiv. class "
                       << "is out-of-order: ";
          reqt.dump(llvm::errs());
          llvm::errs() << "\n";
          abort();
        }

        component.push_back(secondType);
      } else {
        if (!canSig->isReducedType(secondType)) {
          llvm::errs() << "Right hand side is not reduced: ";
          reqt.dump(llvm::errs());
          llvm::errs() << "\n";
          abort();
        }

        if (component.empty()) {
          component.push_back(secondType);
        } else if (!component.back()->isEqual(secondType)) {
          llvm::errs() << "Inconsistent concrete requirement in equiv. class: ";
          reqt.dump(llvm::errs());
          llvm::errs() << "\n";
          abort();
        }
      }
      break;
    }

    case RequirementKind::Conformance:
      // Collect all conformance requirements on each type parameter.
      conformances[CanType(reqt.getFirstType())].push_back(
          reqt.getProtocolDecl());
      break;
    }

    // From here on, we're only interested in requirements beyond the first.
    if (idx == 0) continue;

    // Make sure that the left-hand sides are in nondecreasing order.
    const auto &prevReqt = reqts[idx-1];
    int compareLHS =
      compareDependentTypes(prevReqt.getFirstType(), reqt.getFirstType());
    if (compareLHS > 0) {
      llvm::errs() << "Out-of-order left-hand side: ";
      reqt.dump(llvm::errs());
      llvm::errs() << "\n";
      abort();
    }

    // If we have a concrete same-type requirement, we shouldn't have any
    // other requirements on the same type.
    if (reqt.getKind() == RequirementKind::SameType &&
        !reqt.getSecondType()->isTypeParameter()) {
      if (compareLHS >= 0) {
        llvm::errs() << "Concrete subject type should not have "
                     << "any other requirements: ";
        reqt.dump(llvm::errs());
        llvm::errs() << "\n";
        abort();
      }
    }

    if (prevReqt.compare(reqt) >= 0) {
      llvm::errs() << "Out-of-order requirement: ";
      reqt.dump(llvm::errs());
      llvm::errs() << "\n";
      abort();
    }
  }

  // Make sure we don't have redundant protocol conformance requirements.
  for (const auto &pair : conformances) {
    const auto &protos = pair.second;
    auto canonicalProtos = protos;

    // canonicalizeProtocols() will sort them and filter out any protocols that
    // are refined by other protocols in the list. It should be a no-op at this
    // point.
    ProtocolType::canonicalizeProtocols(canonicalProtos);

    if (protos.size() != canonicalProtos.size()) {
      llvm::errs() << "Redundant conformance requirements in signature\n";
      abort();
    }
    if (!std::equal(protos.begin(), protos.end(), canonicalProtos.begin())) {
      llvm::errs() << "Out-of-order conformance requirements\n";
      abort();
    }
  }

  // Check same-type components for consistency.
  for (const auto &pair : sameTypeComponents) {
    if (pair.second.front()->isTypeParameter() &&
        !canSig->isReducedType(pair.second.front())) {
      llvm::errs() << "Abstract same-type requirement involving concrete types\n";
      llvm::errs() << "Reduced type: " << pair.first << "\n";
      llvm::errs() << "Left hand side of first requirement: "
                   << pair.second.front() << "\n";
      abort();
    }
  }
}

static Type stripBoundDependentMemberTypes(Type t) {
  if (auto *depMemTy = t->getAs<DependentMemberType>()) {
    return DependentMemberType::get(
      stripBoundDependentMemberTypes(depMemTy->getBase()),
      depMemTy->getName());
  }

  return t;
}

static Requirement stripBoundDependentMemberTypes(Requirement req) {
  auto subjectType = stripBoundDependentMemberTypes(req.getFirstType());

  switch (req.getKind()) {
  case RequirementKind::SameShape:
    // Same-shape requirements do not involve dependent member types.
    return req;

  case RequirementKind::Conformance:
    return Requirement(RequirementKind::Conformance, subjectType,
                       req.getSecondType());

  case RequirementKind::Superclass:
  case RequirementKind::SameType:
    return Requirement(req.getKind(), subjectType,
                       req.getSecondType().transform([](Type t) {
                         return stripBoundDependentMemberTypes(t);
                       }));

  case RequirementKind::Layout:
    return Requirement(RequirementKind::Layout, subjectType,
                       req.getLayoutConstraint());
  }

  llvm_unreachable("Bad requirement kind");
}

void swift::validateGenericSignature(ASTContext &context,
                                     GenericSignature sig) {
  llvm::errs() << "Validating generic signature: ";
  sig->print(llvm::errs());
  llvm::errs() << "\n";

  // Try building a new signature having the same requirements.
  SmallVector<GenericTypeParamType *, 2> genericParams;
  for (auto *genericParam :  sig.getGenericParams())
    genericParams.push_back(genericParam);

  SmallVector<Requirement, 2> requirements;
  for (auto requirement : sig.getRequirements())
    requirements.push_back(stripBoundDependentMemberTypes(requirement));

  {
    PrettyStackTraceGenericSignature debugStack("verifying", sig);

    auto newSigWithError = evaluateOrDefault(
        context.evaluator,
        AbstractGenericSignatureRequest{
            nullptr,
            genericParams,
            requirements},
        GenericSignatureWithError());

    // If there were any errors, the signature was invalid.
    auto errorFlags = newSigWithError.getInt();
    if (errorFlags.contains(GenericSignatureErrorFlags::HasInvalidRequirements) ||
        errorFlags.contains(GenericSignatureErrorFlags::CompletionFailed)) {
      context.Diags.diagnose(SourceLoc(), diag::generic_signature_not_valid,
                             sig->getAsString());
    }

    auto newSig = newSigWithError.getPointer();

    // The new signature should be equal.
    if (!newSig->isEqual(sig)) {
      context.Diags.diagnose(SourceLoc(), diag::generic_signature_not_equal,
                             sig->getAsString(), newSig->getAsString());
    }
  }

  // Try removing each requirement in turn.
  for (unsigned victimIndex : indices(requirements)) {
    PrettyStackTraceGenericSignature debugStack("verifying", sig, victimIndex);

    // Add the requirements *except* the victim.
    SmallVector<Requirement, 2> newRequirements;
    for (unsigned i : indices(requirements)) {
      if (i != victimIndex)
        newRequirements.push_back(stripBoundDependentMemberTypes(requirements[i]));
    }

    auto newSigWithError = evaluateOrDefault(
        context.evaluator,
        AbstractGenericSignatureRequest{
          nullptr,
          genericParams,
          newRequirements},
        GenericSignatureWithError());

    // If there were any errors, we formed an invalid signature, so
    // just continue.
    if (newSigWithError.getInt())
      continue;

    auto newSig = newSigWithError.getPointer();

    // If the new signature once again contains the removed requirement, it's
    // not redundant.
    if (newSig->isEqual(sig))
      continue;

    // If the removed requirement is satisfied by the new generic signature,
    // it is redundant. Complain.
    if (newSig->isRequirementSatisfied(requirements[victimIndex])) {
      SmallString<32> reqString;
      {
        llvm::raw_svector_ostream out(reqString);
        requirements[victimIndex].print(out, PrintOptions());
      }
      context.Diags.diagnose(SourceLoc(), diag::generic_signature_not_minimal,
                             reqString, sig->getAsString());
    }
  }
}

void swift::validateGenericSignaturesInModule(ModuleDecl *module) {
  LoadedFile *loadedFile = nullptr;
  for (auto fileUnit : module->getFiles()) {
    loadedFile = dyn_cast<LoadedFile>(fileUnit);
    if (loadedFile) break;
  }

  if (!loadedFile) return;

  // Check all of the (canonical) generic signatures.
  SmallVector<GenericSignature, 8> allGenericSignatures;
  SmallPtrSet<CanGenericSignature, 4> knownGenericSignatures;
  (void)loadedFile->getAllGenericSignatures(allGenericSignatures);
  ASTContext &context = module->getASTContext();
  for (auto genericSig : allGenericSignatures) {
    // Check whether this is the first time we've checked this (canonical)
    // signature.
    auto canGenericSig = genericSig.getCanonicalSignature();
    if (!knownGenericSignatures.insert(canGenericSig).second) continue;

    validateGenericSignature(context, canGenericSig);
  }
}

GenericSignature
swift::buildGenericSignature(ASTContext &ctx,
                             GenericSignature baseSignature,
                             SmallVector<GenericTypeParamType *, 2> addedParameters,
                             SmallVector<Requirement, 2> addedRequirements) {
  return evaluateOrDefault(
      ctx.evaluator,
      AbstractGenericSignatureRequest{
        baseSignature.getPointer(),
        addedParameters,
        addedRequirements},
      GenericSignatureWithError()).getPointer();
}

GenericSignature GenericSignature::withoutMarkerProtocols() const {
  auto requirements = getRequirements();
  SmallVector<Requirement, 4> reducedRequirements;

  // Drop all conformance requirements to marker protocols (if any).
  llvm::copy_if(requirements, std::back_inserter(reducedRequirements),
                [](const Requirement &requirement) {
                  if (requirement.getKind() == RequirementKind::Conformance) {
                    auto *protocol = requirement.getProtocolDecl();
                    return !protocol->isMarkerProtocol();
                  }
                  return true;
                });

  // If nothing changed, let's return this signature back.
  if (requirements.size() == reducedRequirements.size())
    return *this;

  return GenericSignature::get(getGenericParams(), reducedRequirements);
}
