//===--- ProtocolConformance.cpp - AST Protocol Conformance ---------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements the ProtocolConformance class hierarchy.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ProtocolConformance.h"
#include "ConformanceLookupTable.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Availability.h"
#include "swift/AST/Decl.h"
#include "swift/AST/DistributedDecl.h"
#include "swift/AST/FileUnit.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/InFlightSubstitution.h"
#include "swift/AST/LazyResolver.h"
#include "swift/AST/Module.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/PackConformance.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/AST/Types.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/Statistic.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/SaveAndRestore.h"

#define DEBUG_TYPE "AST"

STATISTIC(NumConformanceLookupTables, "# of conformance lookup tables built");

using namespace swift;

Witness::Witness(ValueDecl *decl, SubstitutionMap substitutions,
                 GenericSignature witnessThunkSig,
                 SubstitutionMap reqToWitnessThunkSigSubs,
                 GenericSignature derivativeGenSig,
                 std::optional<ActorIsolation> enterIsolation) {
  if (!witnessThunkSig && substitutions.empty() &&
      reqToWitnessThunkSigSubs.empty() && !enterIsolation) {
    storage = decl;
    return;
  }

  auto &ctx = decl->getASTContext();
  auto declRef = ConcreteDeclRef(decl, substitutions);
  auto storedMem = ctx.Allocate(sizeof(StoredWitness), alignof(StoredWitness));
  auto stored = new (storedMem) StoredWitness{declRef, witnessThunkSig,
                                              reqToWitnessThunkSigSubs,
                                              derivativeGenSig, enterIsolation};

  storage = stored;
}

Witness Witness::withEnterIsolation(ActorIsolation enterIsolation) const {
  return Witness(getDecl(), getSubstitutions(), getWitnessThunkSignature(),
                 getRequirementToWitnessThunkSubs(),
                 getDerivativeGenericSignature(), enterIsolation);
}

void Witness::dump() const { dump(llvm::errs()); }

void Witness::dump(llvm::raw_ostream &out) const {
  out << "Witness: ";
  if (auto decl = this->getDecl()) {
    decl->dumpRef(out);
    out << "\n";
  } else {
    out << "<no decl>\n";
  }
}

#define CONFORMANCE_SUBCLASS_DISPATCH(Method, Args)                          \
switch (getKind()) {                                                         \
  case ProtocolConformanceKind::Normal:                                      \
    return cast<NormalProtocolConformance>(this)->Method Args;               \
  case ProtocolConformanceKind::Self:                                        \
    return cast<SelfProtocolConformance>(this)->Method Args;                 \
  case ProtocolConformanceKind::Specialized:                                 \
    return cast<SpecializedProtocolConformance>(this)->Method Args;          \
  case ProtocolConformanceKind::Inherited:                                   \
    return cast<InheritedProtocolConformance>(this)->Method Args;            \
  case ProtocolConformanceKind::Builtin:                                     \
    assert(&ProtocolConformance::Method != &BuiltinProtocolConformance::Method \
           && "Must override BuiltinProtocolConformance::" #Method);         \
    return cast<BuiltinProtocolConformance>(this)->Method Args;              \
}                                                                            \
llvm_unreachable("bad ProtocolConformanceKind");

#define ROOT_CONFORMANCE_SUBCLASS_DISPATCH(Method, Args)                     \
switch (getKind()) {                                                         \
  case ProtocolConformanceKind::Normal:                                      \
    return cast<NormalProtocolConformance>(this)->Method Args;               \
  case ProtocolConformanceKind::Self:                                        \
    return cast<SelfProtocolConformance>(this)->Method Args;                 \
  case ProtocolConformanceKind::Builtin:                                     \
    return cast<BuiltinProtocolConformance>(this)->Method Args;              \
  case ProtocolConformanceKind::Specialized:                                 \
  case ProtocolConformanceKind::Inherited:                                   \
    llvm_unreachable("not a root conformance");                              \
}                                                                            \
llvm_unreachable("bad ProtocolConformanceKind");

/// Get the protocol being conformed to.
ProtocolDecl *ProtocolConformance::getProtocol() const {
  CONFORMANCE_SUBCLASS_DISPATCH(getProtocol, ())
}

DeclContext *ProtocolConformance::getDeclContext() const {
  CONFORMANCE_SUBCLASS_DISPATCH(getDeclContext, ())
}

/// Retrieve the state of this conformance.
ProtocolConformanceState ProtocolConformance::getState() const {
  CONFORMANCE_SUBCLASS_DISPATCH(getState, ())
}

ConformanceEntryKind ProtocolConformance::getSourceKind() const {
  CONFORMANCE_SUBCLASS_DISPATCH(getSourceKind, ())
}
NormalProtocolConformance *ProtocolConformance::getImplyingConformance() const {
  CONFORMANCE_SUBCLASS_DISPATCH(getImplyingConformance, ())
}

bool
ProtocolConformance::hasTypeWitness(AssociatedTypeDecl *assocType) const {
  CONFORMANCE_SUBCLASS_DISPATCH(hasTypeWitness, (assocType));
}

TypeWitnessAndDecl
ProtocolConformance::getTypeWitnessAndDecl(AssociatedTypeDecl *assocType,
                                           SubstOptions options) const {
  CONFORMANCE_SUBCLASS_DISPATCH(getTypeWitnessAndDecl,
                                (assocType, options))
}

Type ProtocolConformance::getTypeWitness(AssociatedTypeDecl *assocType,
                                         SubstOptions options) const {
  return getTypeWitnessAndDecl(assocType, options).getWitnessType();
}

ConcreteDeclRef
ProtocolConformance::getWitnessDeclRef(ValueDecl *requirement) const {
  CONFORMANCE_SUBCLASS_DISPATCH(getWitnessDeclRef, (requirement))
}

ValueDecl *ProtocolConformance::getWitnessDecl(ValueDecl *requirement) const {
  switch (getKind()) {
  case ProtocolConformanceKind::Normal:
    return cast<NormalProtocolConformance>(this)->getWitness(requirement)
      .getDecl();
  case ProtocolConformanceKind::Self:
    return cast<SelfProtocolConformance>(this)->getWitness(requirement)
      .getDecl();
  case ProtocolConformanceKind::Inherited:
    return cast<InheritedProtocolConformance>(this)
      ->getInheritedConformance()->getWitnessDecl(requirement);

  case ProtocolConformanceKind::Specialized:
    return cast<SpecializedProtocolConformance>(this)
      ->getGenericConformance()->getWitnessDecl(requirement);
  case ProtocolConformanceKind::Builtin:
    return requirement;
  }
  llvm_unreachable("unhandled kind");
}

/// Determine whether the witness for the given requirement
/// is either the default definition or was otherwise deduced.
bool ProtocolConformance::
usesDefaultDefinition(AssociatedTypeDecl *requirement) const {
  CONFORMANCE_SUBCLASS_DISPATCH(usesDefaultDefinition, (requirement))
}

bool ProtocolConformance::isRetroactive() const {
  auto extensionModule = getDeclContext()->getParentModule();
  auto protocolModule = getProtocol()->getParentModule();
  if (extensionModule->isSameModuleLookingThroughOverlays(protocolModule)) {
    return false;
  }

  auto conformingTypeDecl =
      ConformingType->getNominalOrBoundGenericNominal();
  if (conformingTypeDecl) {
    auto conformingTypeModule = conformingTypeDecl->getParentModule();
    if (extensionModule->
        isSameModuleLookingThroughOverlays(conformingTypeModule)) {
      return false;
    }
  }

  return true;
}

GenericEnvironment *ProtocolConformance::getGenericEnvironment() const {
  switch (getKind()) {
  case ProtocolConformanceKind::Inherited:
  case ProtocolConformanceKind::Normal:
  case ProtocolConformanceKind::Self:
    // If we have a normal or inherited protocol conformance, look for its
    // generic parameters.
    return getDeclContext()->getGenericEnvironmentOfContext();

  case ProtocolConformanceKind::Specialized:
  case ProtocolConformanceKind::Builtin:
    // If we have a specialized protocol conformance, since we do not support
    // currently partial specialization, we know that it cannot have any open
    // type variables.
    //
    // FIXME: We could return a meaningful GenericEnvironment here
    return nullptr;
  }

  llvm_unreachable("Unhandled ProtocolConformanceKind in switch.");
}

GenericSignature ProtocolConformance::getGenericSignature() const {
  switch (getKind()) {
  case ProtocolConformanceKind::Inherited:
  case ProtocolConformanceKind::Normal:
  case ProtocolConformanceKind::Self:
    // If we have a normal or inherited protocol conformance, look for its
    // generic signature.
    return getDeclContext()->getGenericSignatureOfContext();

  case ProtocolConformanceKind::Builtin:
    return cast<BuiltinProtocolConformance>(this)->getGenericSignature();

  case ProtocolConformanceKind::Specialized:
    // If we have a specialized protocol conformance, since we do not support
    // currently partial specialization, we know that it cannot have any open
    // type variables.
    return nullptr;
  }

  llvm_unreachable("Unhandled ProtocolConformanceKind in switch.");
}

SubstitutionMap ProtocolConformance::getSubstitutionMap() const {
  CONFORMANCE_SUBCLASS_DISPATCH(getSubstitutionMap, ())
}

SubstitutionMap RootProtocolConformance::getSubstitutionMap() const {
  if (auto genericSig = getGenericSignature())
    return genericSig->getIdentitySubstitutionMap();

  return SubstitutionMap();
}

bool RootProtocolConformance::isInvalid() const {
  ROOT_CONFORMANCE_SUBCLASS_DISPATCH(isInvalid, ())
}

SourceLoc RootProtocolConformance::getLoc() const {
  ROOT_CONFORMANCE_SUBCLASS_DISPATCH(getLoc, ())
}

bool
RootProtocolConformance::isWeakImported(ModuleDecl *fromModule) const {
  auto *dc = getDeclContext();
  if (dc->getParentModule() == fromModule)
    return false;

  // If the protocol is weak imported, so are any conformances to it.
  if (getProtocol()->isWeakImported(fromModule))
    return true;

  // If the conforming type is weak imported, so are any of its conformances.
  if (auto *nominal = getType()->getAnyNominal())
    if (nominal->isWeakImported(fromModule))
      return true;

  // If the conformance is declared in an extension with the @_weakLinked
  // attribute, it is weak imported.
  if (auto *ext = dyn_cast<ExtensionDecl>(dc))
    if (ext->isWeakImported(fromModule))
      return true;

  return false;
}

bool RootProtocolConformance::hasWitness(ValueDecl *requirement) const {
  ROOT_CONFORMANCE_SUBCLASS_DISPATCH(hasWitness, (requirement))
}

bool RootProtocolConformance::isSynthesized() const {
  if (auto normal = dyn_cast<NormalProtocolConformance>(this))
    return normal->isSynthesizedNonUnique() || normal->isConformanceOfProtocol();

  return false;
}

bool NormalProtocolConformance::isRetroactive() const {
  auto module = getDeclContext()->getParentModule();

  // If the conformance occurs in the same module as the protocol definition,
  // this is not a retroactive conformance.
  auto protocolModule = getProtocol()->getDeclContext()->getParentModule();
  if (module == protocolModule)
    return false;

  // If the conformance occurs in the same module as the conforming type
  // definition, this is not a retroactive conformance.
  if (auto nominal = getType()->getAnyNominal()) {
    auto nominalModule = nominal->getParentModule();

    // Consider the overlay module to be the "home" of a nominal type
    // defined in a Clang module.
    if (auto nominalLoadedModule =
          dyn_cast<LoadedFile>(nominal->getModuleScopeContext())) {
      if (auto overlayModule = nominalLoadedModule->getOverlayModule())
        nominalModule = overlayModule;
    }

    if (module == nominalModule)
      return false;
  }

  // Everything else is retroactive.
  return true;
}

bool NormalProtocolConformance::isSynthesizedNonUnique() const {
  // Check if the conformance was synthesized by the ClangImporter.
  if (auto *file = dyn_cast<FileUnit>(getDeclContext()->getModuleScopeContext()))
    return file->getKind() == FileUnitKind::ClangModule;

  return false;
}

bool NormalProtocolConformance::isConformanceOfProtocol() const {
  return getDeclContext()->getSelfProtocolDecl() != nullptr;
}

bool NormalProtocolConformance::isResilient() const {
  // If the type is non-resilient or the module we're in is non-resilient, the
  // conformance is non-resilient.
  // FIXME: Looking at the type is not the right long-term solution. We need an
  // explicit mechanism for declaring conformances as 'fragile', or even
  // individual witnesses.
  if (!getDeclContext()->getSelfNominalTypeDecl()->isResilient())
    return false;

  return getDeclContext()->getParentModule()->isResilient();
}

std::optional<ArrayRef<Requirement>>
ProtocolConformance::getConditionalRequirementsIfAvailable() const {
  CONFORMANCE_SUBCLASS_DISPATCH(getConditionalRequirementsIfAvailable, ());
}

ArrayRef<Requirement> ProtocolConformance::getConditionalRequirements() const {
  CONFORMANCE_SUBCLASS_DISPATCH(getConditionalRequirements, ());
}

std::optional<ArrayRef<Requirement>>
NormalProtocolConformance::getConditionalRequirementsIfAvailable() const {
  const auto &eval = getDeclContext()->getASTContext().evaluator;
  if (eval.hasActiveRequest(ConditionalRequirementsRequest{
          const_cast<NormalProtocolConformance *>(this)})) {
    return std::nullopt;
  }
  return getConditionalRequirements();
}

llvm::ArrayRef<Requirement>
NormalProtocolConformance::getConditionalRequirements() const {
  const auto ext = dyn_cast<ExtensionDecl>(getDeclContext());
  if (ext && ext->isComputingGenericSignature()) {
    return {};
  }
  return evaluateOrDefault(getProtocol()->getASTContext().evaluator,
                           ConditionalRequirementsRequest{
                               const_cast<NormalProtocolConformance *>(this)},
                           {});
}

llvm::ArrayRef<Requirement>
ConditionalRequirementsRequest::evaluate(Evaluator &evaluator,
                                         NormalProtocolConformance *NPC) const {
  // A non-extension conformance won't have conditional requirements.
  const auto ext = dyn_cast<ExtensionDecl>(NPC->getDeclContext());
  if (!ext) {
    return {};
  }

  // If the extension is invalid, it won't ever get a signature, so we
  // "succeed" with an empty result instead.
  if (ext->isInvalid()) {
    return {};
  }

  // A non-generic type won't have conditional requirements.
  const auto typeSig = ext->getExtendedNominal()->getGenericSignature();
  if (!typeSig) {
    return {};
  }

  const auto extensionSig = ext->getGenericSignature();

  // The extension signature should be a superset of the type signature, meaning
  // every thing in the type signature either is included too or is implied by
  // something else. The most important bit is having the same type
  // parameters. (NB. if/when Swift gets parameterized extensions, this needs to
  // change.)
  assert(typeSig.getCanonicalSignature().getGenericParams() ==
         extensionSig.getCanonicalSignature().getGenericParams());

  // Find the requirements in the extension that aren't proved by the original
  // type, these are the ones that make the conformance conditional.
  const auto unsatReqs = extensionSig.requirementsNotSatisfiedBy(typeSig);
  if (unsatReqs.empty())
    return {};

  return NPC->getProtocol()->getASTContext().AllocateCopy(unsatReqs);
}

void NormalProtocolConformance::resolveLazyInfo() const {
  assert(Loader);

  auto *loader = Loader;
  auto *mutableThis = const_cast<NormalProtocolConformance *>(this);
  mutableThis->Loader = nullptr;
  loader->finishNormalConformance(mutableThis, LoaderContextData);
}

void NormalProtocolConformance::setLazyLoader(LazyConformanceLoader *loader,
                                              uint64_t contextData) {
  assert(!Loader && "already has a loader");
  Loader = loader;
  LoaderContextData = contextData;
}

namespace {
  class PrettyStackTraceRequirement : public llvm::PrettyStackTraceEntry {
    const char *Action;
    const ProtocolConformance *Conformance;
    ValueDecl *Requirement;
  public:
    PrettyStackTraceRequirement(const char *action,
                                const ProtocolConformance *conformance,
                                ValueDecl *requirement)
      : Action(action), Conformance(conformance), Requirement(requirement) { }

    void print(llvm::raw_ostream &out) const override {
      out << "While " << Action << " requirement ";
      Requirement->dumpRef(out);
      out << " in conformance ";
      Conformance->printName(out);
      out << "\n";
    }
  };
} // end anonymous namespace

bool NormalProtocolConformance::hasTypeWitness(
                                         AssociatedTypeDecl *assocType) const {
  if (Loader)
    resolveLazyInfo();

  auto found = TypeWitnesses.find(assocType);
  if (found != TypeWitnesses.end()) {
    return !found->getSecond().getWitnessType().isNull();
  }

  return false;
}

TypeWitnessAndDecl
NormalProtocolConformance::getTypeWitnessAndDecl(AssociatedTypeDecl *assocType,
                                                 SubstOptions options) const {
  if (Loader)
    resolveLazyInfo();

  // Check whether we already have a type witness.
  auto known = TypeWitnesses.find(assocType);
  if (known != TypeWitnesses.end())
    return known->second;

  // If there is a tentative-type-witness function, use it.
  if (options.getTentativeTypeWitness) {
   if (Type witnessType =
         Type(options.getTentativeTypeWitness(this, assocType)))
     return { witnessType, nullptr };
  }

  // If this conformance is in a state where it is inferring type witnesses but
  // we didn't find anything, fail.
  //
  // FIXME: This is unsound, because we may not have diagnosed anything but
  // still end up with an ErrorType in the AST.
  if (getDeclContext()->getASTContext().evaluator.hasActiveRequest(
         ResolveTypeWitnessesRequest{
             const_cast<NormalProtocolConformance *>(this)})) {
    return { Type(), nullptr };
  }

  return evaluateOrDefault(
      assocType->getASTContext().evaluator,
      TypeWitnessRequest{const_cast<NormalProtocolConformance *>(this),
                         assocType},
      TypeWitnessAndDecl());
}

TypeWitnessAndDecl NormalProtocolConformance::getTypeWitnessUncached(
    AssociatedTypeDecl *requirement) const {
  auto entry = TypeWitnesses.find(requirement);
  if (entry == TypeWitnesses.end()) {
    return TypeWitnessAndDecl();
  }
  return entry->second;
}

void NormalProtocolConformance::setTypeWitness(AssociatedTypeDecl *assocType,
                                               Type type,
                                               TypeDecl *typeDecl) const {
  assert(getProtocol() == cast<ProtocolDecl>(assocType->getDeclContext()) &&
         "associated type in wrong protocol");
  assert((TypeWitnesses.count(assocType) == 0 ||
          TypeWitnesses[assocType].getWitnessType().isNull()) &&
         "Type witness already known");
  assert((!isComplete() || isInvalid()) && "Conformance already complete?");
  assert(!type->hasArchetype() && "type witnesses must be interface types");
  TypeWitnesses[assocType] = {type, typeDecl};
}

Type ProtocolConformance::getAssociatedType(Type assocType) const {
  assert(assocType->isTypeParameter() &&
         "associated type must be a type parameter");

  ProtocolConformanceRef ref(const_cast<ProtocolConformance*>(this));
  return ref.getAssociatedType(getType(), assocType);
}

ProtocolConformanceRef
ProtocolConformance::getAssociatedConformance(Type assocType,
                                              ProtocolDecl *protocol) const {
  CONFORMANCE_SUBCLASS_DISPATCH(getAssociatedConformance,
                                (assocType, protocol))
}

ProtocolConformanceRef
NormalProtocolConformance::getAssociatedConformance(Type assocType,
                                                 ProtocolDecl *protocol) const {
  if (Loader)
    resolveLazyInfo();

  assert(assocType->isTypeParameter() &&
         "associated type must be a type parameter");

  std::optional<ProtocolConformanceRef> result;

  auto &ctx = getDeclContext()->getASTContext();

  forEachAssociatedConformance(
      [&](Type t, ProtocolDecl *p, unsigned index) {
        if (t->isEqual(assocType) && p == protocol) {
          // Not strictly necessary, but avoids a bit of request evaluator
          // overhead in the happy case.
          if (hasComputedAssociatedConformances()) {
            result = AssociatedConformances[index];
            if (result)
              return true;
          }

          result = evaluateOrDefault(ctx.evaluator,
                                     AssociatedConformanceRequest{
                                       const_cast<NormalProtocolConformance *>(this),
                                       t->getCanonicalType(), p, index
                                     }, ProtocolConformanceRef::forInvalid());
          return true;
        }

        return false;
      });

  assert(result && "Subject type must be exactly equal to left-hand side of a"
         "conformance requirement in protocol requirement signature");
  return *result;
}

/// Allocate the backing array if needed, computing its size from the
///protocol's requirement signature.
void NormalProtocolConformance::createAssociatedConformanceArray() {
  if (hasComputedAssociatedConformances())
    return;

  setHasComputedAssociatedConformances();

  auto *proto = getProtocol();

  unsigned count = 0;
  for (auto req : proto->getRequirementSignature().getRequirements()) {
    if (req.getKind() == RequirementKind::Conformance)
      ++count;
  }

  auto &ctx = proto->getASTContext();
  AssociatedConformances =
      ctx.Allocate<std::optional<ProtocolConformanceRef>>(count);
}

std::optional<ProtocolConformanceRef>
NormalProtocolConformance::getAssociatedConformance(unsigned index) const {
  if (!hasComputedAssociatedConformances())
    return std::nullopt;

  return AssociatedConformances[index];
}

void NormalProtocolConformance::setAssociatedConformance(
    unsigned index, ProtocolConformanceRef assocConf) {
  createAssociatedConformanceArray();

  assert(!AssociatedConformances[index]);
  AssociatedConformances[index] = assocConf;
}

Witness RootProtocolConformance::getWitness(ValueDecl *requirement) const {
  ROOT_CONFORMANCE_SUBCLASS_DISPATCH(getWitness, (requirement))
}

/// Retrieve the value witness corresponding to the given requirement.
Witness NormalProtocolConformance::getWitness(ValueDecl *requirement) const {
  assert(!isa<AssociatedTypeDecl>(requirement) && "Request type witness");
  assert(requirement->isProtocolRequirement() && "Not a requirement");

  if (Loader)
    resolveLazyInfo();

  return evaluateOrDefault(
      requirement->getASTContext().evaluator,
      ValueWitnessRequest{const_cast<NormalProtocolConformance *>(this),
                          requirement},
      Witness());
}

Witness
NormalProtocolConformance::getWitnessUncached(ValueDecl *requirement) const {
  auto entry = Mapping.find(requirement);
  if (entry == Mapping.end()) {
    return Witness();
  }
  return entry->second;
}

Witness SelfProtocolConformance::getWitness(ValueDecl *requirement) const {
  return Witness(requirement, SubstitutionMap(), nullptr, SubstitutionMap(),
                 GenericSignature(), std::nullopt);
}

ConcreteDeclRef
RootProtocolConformance::getWitnessDeclRef(ValueDecl *requirement) const {
  if (auto witness = getWitness(requirement)) {
    auto *witnessDecl = witness.getDecl();

    // If the witness is generic, you have to call getWitness() and build
    // your own substitutions in terms of the witness thunk signature.
    if (auto *witnessDC = dyn_cast<DeclContext>(witnessDecl))
      assert(!witnessDC->isInnermostContextGeneric());

    // If the witness is not generic, use type substitutions from the
    // witness's parent. Don't use witness.getSubstitutions(), which
    // are written in terms of the witness thunk signature.
    auto subs =
      getType()->getContextSubstitutionMap(getDeclContext()->getParentModule(),
                                           witnessDecl->getDeclContext());
    return ConcreteDeclRef(witness.getDecl(), subs);
  }

  return ConcreteDeclRef();
}

void NormalProtocolConformance::setWitness(ValueDecl *requirement,
                                           Witness witness) const {
  assert(!isa<AssociatedTypeDecl>(requirement) && "Request type witness");
  assert(getProtocol() == cast<ProtocolDecl>(requirement->getDeclContext()) &&
         "requirement in wrong protocol");
  assert(Mapping.count(requirement) == 0 && "Witness already known");
  assert((!isComplete() || isInvalid() ||
          // TODO(distributed): properly handle isComplete() for distributed
          //  funcs; there seems to be a problem that we mark completed, but
          //  afterwards will record the thunk witness;
          (dyn_cast<FuncDecl>(requirement)
            ? (dyn_cast<FuncDecl>(requirement)->isDistributed() ||
               dyn_cast<FuncDecl>(requirement)->isDistributedThunk())
            : false) ||
          requirement->getAttrs().hasAttribute<OptionalAttr>() ||
          requirement->getAttrs().isUnavailable(
                                        requirement->getASTContext())) &&
         "Conformance already complete?");
  Mapping[requirement] = witness;
}

void NormalProtocolConformance::overrideWitness(ValueDecl *requirement,
                                                Witness witness) {
  assert(Mapping.count(requirement) == 1 && "Witness not known");
  Mapping[requirement] = witness;
}

void NormalProtocolConformance::resolveValueWitnesses() const {
  auto mutableThis = const_cast<NormalProtocolConformance *>(this);
  evaluateOrDefault(getProtocol()->getASTContext().evaluator,
                    ResolveValueWitnessesRequest{mutableThis},
                    evaluator::SideEffect());
}

SpecializedProtocolConformance::SpecializedProtocolConformance(
    Type conformingType,
    NormalProtocolConformance *genericConformance,
    SubstitutionMap substitutions)
  : ProtocolConformance(ProtocolConformanceKind::Specialized, conformingType),
    GenericConformance(genericConformance),
    GenericSubstitutions(substitutions) {}

void SpecializedProtocolConformance::computeConditionalRequirements() const {
  // already computed?
  if (ConditionalRequirements)
    return;

  auto parentCondReqs =
      GenericConformance->getConditionalRequirementsIfAvailable();
  if (!parentCondReqs)
    return;

  if (!parentCondReqs->empty()) {
    // Substitute the conditional requirements so that they're phrased in
    // terms of the specialized types, not the conformance-declaring decl's
    // types.
    ModuleDecl *module;
    SubstitutionMap subMap;
    if (auto nominal = GenericConformance->getType()->getAnyNominal()) {
      module = nominal->getModuleContext();
      subMap = getType()->getContextSubstitutionMap(module, nominal);
    } else {
      module = getProtocol()->getModuleContext();
      subMap = getSubstitutionMap();
    }

    SmallVector<Requirement, 4> newReqs;
    for (auto oldReq : *parentCondReqs) {
      auto newReq = oldReq.subst(QuerySubstitutionMap{subMap},
                                 LookUpConformanceInModule(module));
      newReqs.push_back(newReq);
    }
    auto &ctxt = getProtocol()->getASTContext();
    ConditionalRequirements = ctxt.AllocateCopy(newReqs);
  } else {
    ConditionalRequirements = ArrayRef<Requirement>();
  }
}

bool SpecializedProtocolConformance::hasTypeWitness(
                                         AssociatedTypeDecl *assocType) const {
  return TypeWitnesses.find(assocType) != TypeWitnesses.end() ||
         GenericConformance->hasTypeWitness(assocType);
}

TypeWitnessAndDecl
SpecializedProtocolConformance::getTypeWitnessAndDecl(
                      AssociatedTypeDecl *assocType, 
                      SubstOptions options) const {
  assert(getProtocol() == cast<ProtocolDecl>(assocType->getDeclContext()) &&
         "associated type in wrong protocol");

  // If we've already computed this type witness, return it.
  auto known = TypeWitnesses.find(assocType);
  if (known != TypeWitnesses.end()) {
    return known->second;
  }

  // Otherwise, perform substitutions to compute this witness.
  auto genericWitnessAndDecl
    = GenericConformance->getTypeWitnessAndDecl(assocType, options);

  auto genericWitness = genericWitnessAndDecl.getWitnessType();
  if (!genericWitness)
    return { Type(), nullptr };

  auto *typeDecl = genericWitnessAndDecl.getWitnessDecl();

  auto substitutionMap = getSubstitutionMap();
  auto specializedType = genericWitness.subst(substitutionMap, options);
  if (specializedType->hasError()) {
    if (options.getTentativeTypeWitness)
      return { Type(), nullptr };

    specializedType = ErrorType::get(genericWitness);
  }

  // Cache the result.
  auto specializedWitnessAndDecl = TypeWitnessAndDecl{specializedType, typeDecl};
  if (!options.getTentativeTypeWitness && !specializedType->hasError())
    TypeWitnesses[assocType] = specializedWitnessAndDecl;

  return specializedWitnessAndDecl;
}

ProtocolConformanceRef
SpecializedProtocolConformance::getAssociatedConformance(Type assocType,
                                                ProtocolDecl *protocol) const {
  ProtocolConformanceRef conformance =
    GenericConformance->getAssociatedConformance(assocType, protocol);

  auto subMap = getSubstitutionMap();

  Type origType =
    (conformance.isConcrete()
       ? conformance.getConcrete()->getType()
       : GenericConformance->getAssociatedType(assocType));

  return conformance.subst(origType, subMap);
}

ConcreteDeclRef
SpecializedProtocolConformance::getWitnessDeclRef(
                                              ValueDecl *requirement) const {
  auto baseWitness = GenericConformance->getWitnessDeclRef(requirement);
  if (!baseWitness || !baseWitness.isSpecialized())
    return baseWitness;

  auto specializationMap = getSubstitutionMap();

  auto witnessDecl = baseWitness.getDecl();
  auto witnessMap = baseWitness.getSubstitutions();

  auto combinedMap = witnessMap.subst(specializationMap);
  return ConcreteDeclRef(witnessDecl, combinedMap);
}

ProtocolConformanceRef
InheritedProtocolConformance::getAssociatedConformance(Type assocType,
                                                ProtocolDecl *protocol) const {
  auto underlying =
    InheritedConformance->getAssociatedConformance(assocType, protocol);


  // If the conformance is for Self, return an inherited conformance.
  if (underlying.isConcrete() &&
      assocType->isEqual(getProtocol()->getSelfInterfaceType())) {
    auto subclassType = getType();
    ASTContext &ctx = subclassType->getASTContext();
    return ProtocolConformanceRef(
             ctx.getInheritedConformance(subclassType,
                                         underlying.getConcrete()));
  }

  return underlying;
}

ConcreteDeclRef
InheritedProtocolConformance::getWitnessDeclRef(ValueDecl *requirement) const {
  // FIXME: substitutions?
  return InheritedConformance->getWitnessDeclRef(requirement);
}

const NormalProtocolConformance *
ProtocolConformance::getRootNormalConformance() const {
  // This is an unsafe cast; remove this entire method.
  return cast<NormalProtocolConformance>(getRootConformance());
}

const RootProtocolConformance *
ProtocolConformance::getRootConformance() const {
  const ProtocolConformance *C = this;
  if (auto *inheritedC = dyn_cast<InheritedProtocolConformance>(C))
    C = inheritedC->getInheritedConformance();
  if (auto *specializedC = dyn_cast<SpecializedProtocolConformance>(C))
    return specializedC->getGenericConformance();

  return cast<RootProtocolConformance>(C);
}

bool ProtocolConformance::isVisibleFrom(const DeclContext *dc) const {
  // FIXME: Implement me!
  return true;
}

ProtocolConformanceRef
ProtocolConformance::subst(SubstitutionMap subMap,
                           SubstOptions options) const {
  InFlightSubstitutionViaSubMap IFS(subMap, options);
  return subst(IFS);
}

ProtocolConformanceRef
ProtocolConformance::subst(TypeSubstitutionFn subs,
                           LookupConformanceFn conformances,
                           SubstOptions options) const {
  InFlightSubstitution IFS(subs, conformances, options);
  return subst(IFS);
}

/// Check if the replacement is a one-element pack with a scalar type.
static bool isVanishingTupleConformance(
    NormalProtocolConformance *generic,
    SubstitutionMap substitutions) {
  if (!isa<BuiltinTupleDecl>(generic->getDeclContext()->getSelfNominalTypeDecl()))
    return false;

  auto replacementTypes = substitutions.getReplacementTypes();
  assert(replacementTypes.size() == 1);
  auto packType = replacementTypes[0]->castTo<PackType>();

  return (packType->getNumElements() == 1 &&
          !packType->getElementTypes()[0]->is<PackExpansionType>());
}

/// Don't form a tuple conformance if the substituted type is unwrapped
/// from a one-element tuple.
///
/// That is, [(repeat each T): P] ⊗ {each T := Pack{U};
///                                  [each T: P]: Pack{ [U: P] }}
///                 => [U: P]
static ProtocolConformanceRef unwrapVanishingTupleConformance(
    SubstitutionMap substitutions) {
  auto conformances = substitutions.getConformances();
  assert(conformances.size() == 1);
  assert(conformances[0].isPack());
  auto packConformance = conformances[0].getPack();

  assert(packConformance->getPatternConformances().size() == 1);
  return packConformance->getPatternConformances()[0];
}

ProtocolConformanceRef
ProtocolConformance::subst(InFlightSubstitution &IFS) const {
  auto *mutableThis = const_cast<ProtocolConformance *>(this);

  switch (getKind()) {
  case ProtocolConformanceKind::Normal: {
    auto origType = getType();
    if (!origType->hasTypeParameter() &&
        !origType->hasArchetype())
      return ProtocolConformanceRef(mutableThis);

    auto substType = origType.subst(IFS);
    if (substType->isEqual(origType))
      return ProtocolConformanceRef(mutableThis);

    auto *generic = cast<NormalProtocolConformance>(mutableThis);
    auto subMap = SubstitutionMap::get(getGenericSignature(), IFS);

    if (isVanishingTupleConformance(generic, subMap))
      return unwrapVanishingTupleConformance(subMap);

    auto &ctx = substType->getASTContext();
    auto *concrete = ctx.getSpecializedConformance(substType, generic, subMap);

    return ProtocolConformanceRef(concrete);
  }

  case ProtocolConformanceKind::Builtin: {
    auto origType = getType();
    if (!origType->hasTypeParameter() &&
        !origType->hasArchetype())
      return ProtocolConformanceRef(mutableThis);

    auto substType = origType.subst(IFS);

    // We do an exact pointer equality check because subst() can
    // change sugar.
    if (substType.getPointer() == origType.getPointer())
      return ProtocolConformanceRef(mutableThis);

    auto kind = cast<BuiltinProtocolConformance>(this)
        ->getBuiltinConformanceKind();

    auto *concrete = substType->getASTContext()
        .getBuiltinConformance(substType, getProtocol(), kind);
    return ProtocolConformanceRef(concrete);
  }

  case ProtocolConformanceKind::Self:
    return ProtocolConformanceRef(mutableThis);

  case ProtocolConformanceKind::Inherited: {
    // Substitute the base.
    auto inheritedConformance
      = cast<InheritedProtocolConformance>(this)->getInheritedConformance();

    auto origType = getType();
    if (!origType->hasTypeParameter() &&
        !origType->hasArchetype()) {
      return ProtocolConformanceRef(mutableThis);
    }

    auto origBaseType = inheritedConformance->getType();
    if (origBaseType->hasTypeParameter() ||
        origBaseType->hasArchetype()) {
      // Substitute into the superclass.
      auto substConformance = inheritedConformance->subst(IFS);
      if (!substConformance.isConcrete())
        return substConformance;

      inheritedConformance = substConformance.getConcrete();
    }

    auto substType = origType.subst(IFS);
    auto *concrete = substType->getASTContext()
      .getInheritedConformance(substType, inheritedConformance);
    return ProtocolConformanceRef(concrete);
  }

  case ProtocolConformanceKind::Specialized: {
    // Substitute the substitutions in the specialized conformance.
    auto spec = cast<SpecializedProtocolConformance>(this);

    auto *generic = spec->getGenericConformance();
    auto subMap = spec->getSubstitutionMap().subst(IFS);

    if (isVanishingTupleConformance(generic, subMap))
      return unwrapVanishingTupleConformance(subMap);

    auto substType = spec->getType().subst(IFS);

    auto &ctx = substType->getASTContext();
    auto *concrete = ctx.getSpecializedConformance(substType, generic, subMap);

    return ProtocolConformanceRef(concrete);
  }
  }
  llvm_unreachable("bad ProtocolConformanceKind");
}

ProtocolConformance *
ProtocolConformance::getInheritedConformance(ProtocolDecl *protocol) const {
  auto result =
    getAssociatedConformance(getProtocol()->getSelfInterfaceType(), protocol);
  return result.isConcrete() ? result.getConcrete() : nullptr;
}

#pragma mark Protocol conformance lookup
void NominalTypeDecl::prepareConformanceTable() const {
  assert(!isa<ProtocolDecl>(this) &&
         "Protocols don't have a conformance table");

  if (ConformanceTable)
    return;

  auto mutableThis = const_cast<NominalTypeDecl *>(this);
  ASTContext &ctx = getASTContext();
  ConformanceTable = new (ctx) ConformanceLookupTable(ctx);
  ++NumConformanceLookupTables;

  // If this type declaration was not parsed from source code or introduced
  // via the Clang importer, don't add any synthesized conformances.
  auto *file = cast<FileUnit>(getModuleScopeContext());
  if (file->getKind() != FileUnitKind::Source &&
      file->getKind() != FileUnitKind::ClangModule &&
      file->getKind() != FileUnitKind::DWARFModule &&
      file->getKind() != FileUnitKind::Synthesized) {
    return;
  }

  SmallPtrSet<ProtocolDecl *, 2> protocols;
  auto addSynthesized = [&](ProtocolDecl *proto) {
    if (!proto)
      return;

    if (protocols.count(proto) == 0) {
      ConformanceTable->addSynthesizedConformance(
          mutableThis, proto, mutableThis);
      protocols.insert(proto);
    }
  };

  // Synthesize the unconditional conformances to invertible protocols.
  // FIXME: We should be able to only resolve the inheritance clause once,
  // but we also do it in ConformanceLookupTable::updateLookupTable().
  InvertibleProtocolSet inverses;
  bool anyObject = false;
  (void) getDirectlyInheritedNominalTypeDecls(this, inverses, anyObject);

  // Handle deprecated attributes.
  if (getAttrs().hasAttribute<MoveOnlyAttr>())
    inverses.insert(InvertibleProtocolKind::Copyable);
  if (getAttrs().hasAttribute<NonEscapableAttr>())
    inverses.insert(InvertibleProtocolKind::Escapable);

  bool hasSuppressedConformances = false;
  for (auto ip : InvertibleProtocolSet::allKnown()) {
    if (!inverses.contains(ip) ||
        (isa<ClassDecl>(this) &&
         !ctx.LangOpts.hasFeature(Feature::MoveOnlyClasses))) {
      addSynthesized(ctx.getProtocol(getKnownProtocolKind(ip)));
    } else {
      hasSuppressedConformances = true;
    }
  }

  // Non-copyable and non-escaping types do not implicitly conform to
  // any other protocols.
  if (hasSuppressedConformances)
    return;

  // Don't do any more for synthesized FileUnits.
  if (file->getKind() == FileUnitKind::Synthesized)
    return;

  // Add protocols for any synthesized protocol attributes.
  for (auto attr : getAttrs().getAttributes<SynthesizedProtocolAttr>()) {
    addSynthesized(attr->getProtocol());
  }

  // Add any implicit conformances.
  if (auto theEnum = dyn_cast<EnumDecl>(mutableThis)) {
    if (theEnum->hasCases() && theEnum->hasOnlyCasesWithoutAssociatedValues()) {
      // Simple enumerations conform to Equatable.
      addSynthesized(ctx.getProtocol(KnownProtocolKind::Equatable));

      // Simple enumerations conform to Hashable.
      addSynthesized(ctx.getProtocol(KnownProtocolKind::Hashable));
    }

    // Enumerations with a raw type conform to RawRepresentable.
    if (theEnum->hasRawType() && !theEnum->getRawType()->hasError()) {
      addSynthesized(ctx.getProtocol(KnownProtocolKind::RawRepresentable));
    }
  }

  // Actor classes conform to the actor protocol.
  if (auto classDecl = dyn_cast<ClassDecl>(mutableThis)) {
    if (classDecl->isDistributedActor()) {
      addSynthesized(ctx.getProtocol(KnownProtocolKind::DistributedActor));
    } else if (classDecl->isActor()) {
      addSynthesized(ctx.getProtocol(KnownProtocolKind::Actor));
    }
  }

  // Global actors conform to the GlobalActor protocol.
  if (mutableThis->getAttrs().hasAttribute<GlobalActorAttr>()) {
    addSynthesized(ctx.getProtocol(KnownProtocolKind::GlobalActor));
  }
}

/// Handle special cased protocol-to-protocol conformances, such as e.g.
/// DistributedActor-to-Actor.
///
/// \returns true when the conformance lookup was handled successfully
static bool lookupSpecialProtocolToProtocolConformance(
    const ProtocolDecl *thisProtocol, ProtocolDecl *toProtocol,
    SmallVectorImpl<ProtocolConformance *> &conformances) {
  auto &C = toProtocol->getASTContext();

  // DistributedActor-to-Actor
  if (thisProtocol->isSpecificProtocol(KnownProtocolKind::DistributedActor) &&
      toProtocol->isSpecificProtocol(KnownProtocolKind::Actor)) {
    if (auto conformance = getDistributedActorAsActorConformance(C)) {
      conformances.push_back(conformance);
      return true;
    }
  }

  return false;
}

bool NominalTypeDecl::lookupConformance(
       ProtocolDecl *protocol,
       SmallVectorImpl<ProtocolConformance *> &conformances) const {

  // In general, protocols cannot conform to other protocols, however there are
  // exceptions, special handle those.
  if (auto thisProtocol = dyn_cast<ProtocolDecl>(this)) {
    if (lookupSpecialProtocolToProtocolConformance(thisProtocol, protocol,
                                                   conformances)) {
      return true;
    }
  }

  assert(!isa<ProtocolDecl>(this) &&
         "Self-conformances are only found by the higher-level "
         "ModuleDecl::lookupConformance() entry point");

  prepareConformanceTable();
  return ConformanceTable->lookupConformance(
           const_cast<NominalTypeDecl *>(this),
           protocol,
           conformances);
}

SmallVector<ProtocolDecl *, 2>
NominalTypeDecl::getAllProtocols(bool sorted) const {
  assert(!isa<ProtocolDecl>(this) &&
         "For inherited protocols, use ProtocolDecl::inheritsFrom() or "
         "ProtocolDecl::getInheritedProtocols()");

  prepareConformanceTable();
  SmallVector<ProtocolDecl *, 2> result;
  ConformanceTable->getAllProtocols(const_cast<NominalTypeDecl *>(this), result,
                                    sorted);
  return result;
}

SmallVector<ProtocolConformance *, 2> NominalTypeDecl::getAllConformances(
                                        bool sorted) const
{
  prepareConformanceTable();
  SmallVector<ProtocolConformance *, 2> result;
  ConformanceTable->getAllConformances(const_cast<NominalTypeDecl *>(this),
                                       sorted,
                                       result);
  return result;
}

void NominalTypeDecl::getImplicitProtocols(
       SmallVectorImpl<ProtocolDecl *> &protocols) {
  prepareConformanceTable();
  ConformanceTable->getImplicitProtocols(this, protocols);
}

void NominalTypeDecl::registerProtocolConformance(
    NormalProtocolConformance *conformance, bool synthesized) {
  prepareConformanceTable();
  auto *dc = conformance->getDeclContext();
  ConformanceTable->registerProtocolConformance(dc, conformance, synthesized);
}

ArrayRef<ValueDecl *>
NominalTypeDecl::getSatisfiedProtocolRequirementsForMember(
                                             const ValueDecl *member,
                                             bool sorted) const {
  assert(member->getDeclContext()->getSelfNominalTypeDecl() == this);
  assert(!isa<ProtocolDecl>(this));
  prepareConformanceTable();
  return ConformanceTable->getSatisfiedProtocolRequirementsForMember(member,
                                           const_cast<NominalTypeDecl *>(this),
                                           sorted);
}

SmallVector<ProtocolDecl *, 2>
IterableDeclContext::getLocalProtocols(ConformanceLookupKind lookupKind) const {
  SmallVector<ProtocolDecl *, 2> result;
  for (auto conformance : getLocalConformances(lookupKind))
    result.push_back(conformance->getProtocol());
  return result;
}



/// Find a synthesized conformance in this declaration context, if there is one.
static ProtocolConformance *
findSynthesizedConformance(
    const DeclContext *dc,
    KnownProtocolKind protoKind) {
  auto nominal = dc->getSelfNominalTypeDecl();

  // Perform some common checks
  if (!nominal)
    return nullptr;

  if (dc->getParentModule() != nominal->getParentModule())
    return nullptr;

  auto &C = nominal->getASTContext();
  auto cvProto = C.getProtocol(protoKind);
  if (!cvProto)
    return nullptr;

  auto module = dc->getParentModule();
  auto conformance = module->lookupConformance(
      nominal->getDeclaredInterfaceType(), cvProto);
  if (!conformance || !conformance.isConcrete())
    return nullptr;

  auto concrete = conformance.getConcrete();
  if (concrete->getDeclContext() != dc)
    return nullptr;

  if (isa<InheritedProtocolConformance>(concrete))
    return nullptr;

  auto normal = concrete->getRootNormalConformance();
  if (!normal || normal->getSourceKind() != ConformanceEntryKind::Synthesized)
    return nullptr;

  return normal;
}

/// Find any synthesized conformances for given decl context.
///
/// Some protocol conformances can be synthesized by the compiler,
/// for those, we need to add them to "local conformances" because otherwise
/// we'd get missing symbols while attempting to use these.
static SmallVector<ProtocolConformance *, 2> findSynthesizedConformances(
    const DeclContext *dc) {
  auto nominal = dc->getSelfNominalTypeDecl();
  if (!nominal)
    return {};

  // Try to find specific conformances
  SmallVector<ProtocolConformance *, 2> result;

  auto trySynthesize = [&](KnownProtocolKind knownProto) {
    if (auto conformance =
            findSynthesizedConformance(dc, knownProto)) {
      result.push_back(conformance);
    }
  };

  // Concrete types may synthesize some conformances
  if (!isa<ProtocolDecl>(nominal)) {
    trySynthesize(KnownProtocolKind::Sendable);

    // Triggers synthesis of a possibly conditional conformance.
    // For the unconditional ones, see NominalTypeDecl::prepareConformanceTable
    for (auto ip : InvertibleProtocolSet::allKnown())
      trySynthesize(getKnownProtocolKind(ip));

    trySynthesize(KnownProtocolKind::BitwiseCopyable);
  }

  /// Distributed actors can synthesize Encodable/Decodable, so look for those
  if (canSynthesizeDistributedActorCodableConformance(nominal)) {
    trySynthesize(KnownProtocolKind::Encodable);
    trySynthesize(KnownProtocolKind::Decodable);
  }

  return result;
}

std::vector<ProtocolConformance *>
LookupAllConformancesInContextRequest::evaluate(
    Evaluator &eval, const IterableDeclContext *IDC) const {
  // Dig out the nominal type.
  const auto dc = IDC->getAsGenericContext();
  const auto nominal = dc->getSelfNominalTypeDecl();
  if (!nominal) {
    return { };
  }

  // Protocols only have self-conformances.
  if (auto protocol = dyn_cast<ProtocolDecl>(nominal)) {
    if (protocol->requiresSelfConformanceWitnessTable()) {
      return { protocol->getASTContext().getSelfConformance(protocol) };
    }

    return { };
  }

  // Record all potential conformances.
  nominal->prepareConformanceTable();
  std::vector<ProtocolConformance *> conformances;
  nominal->ConformanceTable->lookupConformances(
    nominal,
    const_cast<GenericContext *>(dc),
    &conformances,
    nullptr);

  return conformances;
}

SmallVector<ProtocolConformance *, 2>
IterableDeclContext::getLocalConformances(ConformanceLookupKind lookupKind)
    const {
  // Look up the cached set of all of the conformances.
  std::vector<ProtocolConformance *> conformances =
      evaluateOrDefault(
        getASTContext().evaluator, LookupAllConformancesInContextRequest{this},
        { });

  // Copy all of the conformances we want.
  SmallVector<ProtocolConformance *, 2> result;
  std::copy_if(
      conformances.begin(), conformances.end(), std::back_inserter(result),
      [&](ProtocolConformance *conformance) {
         // If we are to filter out this result, do so now.
         switch (lookupKind) {
         case ConformanceLookupKind::OnlyExplicit:
           switch (conformance->getSourceKind()) {
           case ConformanceEntryKind::Explicit:
           case ConformanceEntryKind::Synthesized:
               return true;
           case ConformanceEntryKind::PreMacroExpansion:
           case ConformanceEntryKind::Implied:
           case ConformanceEntryKind::Inherited:
             return false;
           }

         case ConformanceLookupKind::NonInherited:
           switch (conformance->getSourceKind()) {
           case ConformanceEntryKind::Explicit:
           case ConformanceEntryKind::Synthesized:
           case ConformanceEntryKind::Implied:
             return true;
           case ConformanceEntryKind::Inherited:
           case ConformanceEntryKind::PreMacroExpansion:
             return false;
           }

         case ConformanceLookupKind::All:
         case ConformanceLookupKind::NonStructural:
           return true;
         }
      });

  // If we want to add structural conformances, do so now.
  switch (lookupKind) {
    case ConformanceLookupKind::All:
    case ConformanceLookupKind::NonInherited: {
      // Look for a Sendable conformance globally. If it is synthesized
      // and matches this declaration context, use it.
      auto dc = getAsGenericContext();

      SmallPtrSet<ProtocolConformance *, 4> known;
      for (auto conformance : findSynthesizedConformances(dc)) {
        // Compute the known set of conformances for the first time.
        if (known.empty()) {
          known.insert(result.begin(), result.end());
        }

        if (known.insert(conformance).second)
          result.push_back(conformance);
      }
      break;
    }

    case ConformanceLookupKind::NonStructural:
    case ConformanceLookupKind::OnlyExplicit:
      break;
  }

  return result;
}

SmallVector<ConformanceDiagnostic, 4>
IterableDeclContext::takeConformanceDiagnostics() const {
  SmallVector<ConformanceDiagnostic, 4> result;

  // Dig out the nominal type.
  const auto dc = getAsGenericContext();
  const auto nominal = dc->getSelfNominalTypeDecl();

  if (!nominal) {
    return result;
  }

  // Protocols are not subject to the checks for supersession.
  if (isa<ProtocolDecl>(nominal)) {
    return result;
  }

  // Update to record all potential conformances.
  nominal->prepareConformanceTable();
  nominal->ConformanceTable->lookupConformances(
    nominal,
    const_cast<GenericContext *>(dc),
    nullptr,
    &result);

  return result;
}

/// Check of all types used by the conformance are canonical.
bool ProtocolConformance::isCanonical() const {
  // Normal conformances are always canonical by construction.
  if (getKind() == ProtocolConformanceKind::Normal)
    return true;

  if (!getType()->isCanonical())
    return false;

  switch (getKind()) {
  case ProtocolConformanceKind::Self:
  case ProtocolConformanceKind::Normal: {
    return true;
  }
  case ProtocolConformanceKind::Builtin: {
    // FIXME: Not the conforming type?
    return true;
  }
  case ProtocolConformanceKind::Inherited: {
    // Substitute the base.
    auto inheritedConformance
      = cast<InheritedProtocolConformance>(this);
    return inheritedConformance->getInheritedConformance()->isCanonical();
  }
  case ProtocolConformanceKind::Specialized: {
    // Substitute the substitutions in the specialized conformance.
    auto spec = cast<SpecializedProtocolConformance>(this);
    auto genericConformance = spec->getGenericConformance();
    if (!genericConformance->isCanonical())
      return false;
    if (!spec->getSubstitutionMap().isCanonical()) return false;
    return true;
  }
  }
  llvm_unreachable("bad ProtocolConformanceKind");
}

/// Check of all types used by the conformance are canonical.
ProtocolConformance *ProtocolConformance::getCanonicalConformance() {
  if (isCanonical())
    return this;

  switch (getKind()) {
  case ProtocolConformanceKind::Self:
  case ProtocolConformanceKind::Normal: {
    // Root conformances are always canonical by construction.
    return this;
  }
  case ProtocolConformanceKind::Builtin: {
    // Canonicalize the subject type of the builtin conformance.
    auto &Ctx = getType()->getASTContext();
    auto builtinConformance = cast<BuiltinProtocolConformance>(this);
    return Ctx.getBuiltinConformance(
      builtinConformance->getType()->getCanonicalType(),
      builtinConformance->getProtocol(),
      builtinConformance->getBuiltinConformanceKind());
  }

  case ProtocolConformanceKind::Inherited: {
    auto &Ctx = getType()->getASTContext();
    auto inheritedConformance = cast<InheritedProtocolConformance>(this);
    return Ctx.getInheritedConformance(
        getType()->getCanonicalType(),
        inheritedConformance->getInheritedConformance()
            ->getCanonicalConformance());
  }

  case ProtocolConformanceKind::Specialized: {
    auto &Ctx = getType()->getASTContext();
    // Substitute the substitutions in the specialized conformance.
    auto spec = cast<SpecializedProtocolConformance>(this);
    auto genericConformance = spec->getGenericConformance();
    return Ctx.getSpecializedConformance(
                                getType()->getCanonicalType(),
                                cast<NormalProtocolConformance>(
                                    genericConformance->getCanonicalConformance()),
                                spec->getSubstitutionMap().getCanonical());
  }
  }
  llvm_unreachable("bad ProtocolConformanceKind");
}

BuiltinProtocolConformance::BuiltinProtocolConformance(
    Type conformingType, ProtocolDecl *protocol, BuiltinConformanceKind kind)
    : RootProtocolConformance(ProtocolConformanceKind::Builtin, conformingType),
      protocol(protocol) {
  Bits.BuiltinProtocolConformance.Kind = unsigned(kind);
}

// See swift/Basic/Statistic.h for declaration: this enables tracing
// ProtocolConformances, is defined here to avoid too much layering violation /
// circular linkage dependency.

struct ProtocolConformanceTraceFormatter
    : public UnifiedStatsReporter::TraceFormatter {
  void traceName(const void *Entity, raw_ostream &OS) const override {
    if (!Entity)
      return;
    const ProtocolConformance *C =
        static_cast<const ProtocolConformance *>(Entity);
    C->printName(OS);
  }
  void traceLoc(const void *Entity, SourceManager *SM,
                clang::SourceManager *CSM, raw_ostream &OS) const override {
    if (!Entity)
      return;
    const ProtocolConformance *C =
        static_cast<const ProtocolConformance *>(Entity);
    if (auto const *NPC = dyn_cast<NormalProtocolConformance>(C)) {
      NPC->getLoc().print(OS, *SM);
    } else if (auto const *DC = C->getDeclContext()) {
      if (auto const *D = DC->getAsDecl())
        D->getLoc().print(OS, *SM);
    }
  }
};

static ProtocolConformanceTraceFormatter TF;

template<>
const UnifiedStatsReporter::TraceFormatter*
FrontendStatsTracer::getTraceFormatter<const ProtocolConformance *>() {
  return &TF;
}

void swift::simple_display(llvm::raw_ostream &out,
                           const ProtocolConformance *conf) {
  conf->printName(out);
}

SourceLoc swift::extractNearestSourceLoc(const ProtocolConformance *conformance) {
  return extractNearestSourceLoc(conformance->getDeclContext());
}
