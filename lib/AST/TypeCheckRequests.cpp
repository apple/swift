//===--- TypeCheckRequests.cpp - Type Checking Requests ------------------===//
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
#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/DiagnosticsCommon.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/Module.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/PropertyWrappers.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/RequirementSignature.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/AST/TypeLoc.h"
#include "swift/AST/TypeRepr.h"
#include "swift/AST/Types.h"
#include "swift/Subsystems.h"

#include "llvm/ADT/None.h"
#include "llvm/ADT/Optional.h"

using namespace swift;

namespace swift {
// Implement the type checker type zone (zone 10).
#define SWIFT_TYPEID_ZONE TypeChecker
#define SWIFT_TYPEID_HEADER "swift/AST/TypeCheckerTypeIDZone.def"
#include "swift/Basic/ImplementTypeIDZone.h"
#undef SWIFT_TYPEID_ZONE
#undef SWIFT_TYPEID_HEADER
}

void swift::simple_display(
    llvm::raw_ostream &out,
    const llvm::PointerUnion<const TypeDecl *, const ExtensionDecl *> &value) {
  if (auto type = value.dyn_cast<const TypeDecl *>()) {
    type->dumpRef(out);
    return;
  }

  auto ext = value.get<const ExtensionDecl *>();
  simple_display(out, ext);
}

void swift::simple_display(llvm::raw_ostream &out, ASTContext *ctx) {
  out << "(AST Context)";
}

void swift::simple_display(llvm::raw_ostream &out,
                           const TypeResolutionStage &value) {
  switch (value) {
  case TypeResolutionStage::Structural:
    out << "structural";
    break;

  case TypeResolutionStage::Interface:
    out << "interface";
    break;
  }
}

void swift::simple_display(llvm::raw_ostream &out, ASTNode node) {
  if (node) {
    node.dump(out);
  } else {
    out << "null";
  }
}

void swift::simple_display(llvm::raw_ostream &out, Type type) {
  if (type)
    type.print(out);
  else
    out << "null";
}

void swift::simple_display(llvm::raw_ostream &out, const TypeRepr *TyR) {
  if (TyR)
    TyR->print(out);
  else
    out << "null";
}

void swift::simple_display(llvm::raw_ostream &out, const TypeLoc source) {
  out << "(";
  simple_display(out, source.getType());
  out << ", ";
  simple_display(out, source.getTypeRepr());
  out << ")";
}

//----------------------------------------------------------------------------//
// Inherited type computation.
//----------------------------------------------------------------------------//

SourceLoc InheritedTypeRequest::getNearestLoc() const {
  const auto &storage = getStorage();
  auto &typeLoc = getInheritedTypeLocAtIndex(std::get<0>(storage),
                                             std::get<1>(storage));
  return typeLoc.getLoc();
}

bool InheritedTypeRequest::isCached() const {
  return std::get<2>(getStorage()) == TypeResolutionStage::Interface;
}

llvm::Optional<Type> InheritedTypeRequest::getCachedResult() const {
  const auto &storage = getStorage();
  auto &typeLoc = getInheritedTypeLocAtIndex(std::get<0>(storage),
                                             std::get<1>(storage));
  if (typeLoc.wasValidated())
    return typeLoc.getType();

  return llvm::None;
}

void InheritedTypeRequest::cacheResult(Type value) const {
  const auto &storage = getStorage();
  auto &typeLoc = getInheritedTypeLocAtIndex(std::get<0>(storage),
                                             std::get<1>(storage));
  const_cast<TypeLoc &>(typeLoc).setType(value);
}

//----------------------------------------------------------------------------//
// Superclass computation.
//----------------------------------------------------------------------------//
void SuperclassTypeRequest::diagnoseCycle(DiagnosticEngine &diags) const {
  // FIXME: Improve this diagnostic.
  auto nominalDecl = std::get<0>(getStorage());
  diags.diagnose(nominalDecl, diag::circular_class_inheritance,
                 nominalDecl->getName());
}

bool SuperclassTypeRequest::isCached() const {
  return std::get<1>(getStorage()) == TypeResolutionStage::Interface;
}

llvm::Optional<Type> SuperclassTypeRequest::getCachedResult() const {
  auto nominalDecl = std::get<0>(getStorage());

  if (auto *classDecl = dyn_cast<ClassDecl>(nominalDecl))
    if (classDecl->LazySemanticInfo.SuperclassType.getInt())
      return classDecl->LazySemanticInfo.SuperclassType.getPointer();

  if (auto *protocolDecl = dyn_cast<ProtocolDecl>(nominalDecl))
    if (protocolDecl->LazySemanticInfo.SuperclassType.getInt())
      return protocolDecl->LazySemanticInfo.SuperclassType.getPointer();

  return llvm::None;
}

void SuperclassTypeRequest::cacheResult(Type value) const {
  auto nominalDecl = std::get<0>(getStorage());

  if (auto *classDecl = dyn_cast<ClassDecl>(nominalDecl))
    classDecl->LazySemanticInfo.SuperclassType.setPointerAndInt(value, true);

  if (auto *protocolDecl = dyn_cast<ProtocolDecl>(nominalDecl))
    protocolDecl->LazySemanticInfo.SuperclassType.setPointerAndInt(value, true);
}

void SuperclassTypeRequest::writeDependencySink(
    evaluator::DependencyCollector &tracker, Type value) const {
  if (!value)
    return;

  // FIXME: This is compatible with the existing name tracking scheme, but
  // ignoring this name when we fail to look up a class is bogus.
  ClassDecl *Super = value->getClassOrBoundGenericClass();
  if (!Super)
    return;
  tracker.addPotentialMember(Super);
}

//----------------------------------------------------------------------------//
// Enum raw type computation.
//----------------------------------------------------------------------------//
void EnumRawTypeRequest::diagnoseCycle(DiagnosticEngine &diags) const {
  // FIXME: Improve this diagnostic.
  auto enumDecl = std::get<0>(getStorage());
  diags.diagnose(enumDecl, diag::circular_enum_inheritance, enumDecl->getName());
}

void EnumRawTypeRequest::noteCycleStep(DiagnosticEngine &diags) const {
  auto *decl = std::get<0>(getStorage());
  diags.diagnose(decl, diag::kind_declname_declared_here,
                 decl->getDescriptiveKind(), decl->getName());
}

//----------------------------------------------------------------------------//
// isObjC computation.
//----------------------------------------------------------------------------//

llvm::Optional<bool> IsObjCRequest::getCachedResult() const {
  auto decl = std::get<0>(getStorage());
  if (decl->LazySemanticInfo.isObjCComputed)
    return decl->LazySemanticInfo.isObjC;

  return llvm::None;
}

void IsObjCRequest::cacheResult(bool value) const {
  auto decl = std::get<0>(getStorage());
  decl->setIsObjC(value);
}

//----------------------------------------------------------------------------//
// requiresClass computation.
//----------------------------------------------------------------------------//

void ProtocolRequiresClassRequest::diagnoseCycle(DiagnosticEngine &diags) const {
  auto decl = std::get<0>(getStorage());
  diags.diagnose(decl, diag::circular_protocol_def, decl->getName());
}

void ProtocolRequiresClassRequest::noteCycleStep(DiagnosticEngine &diags) const {
  auto requirement = std::get<0>(getStorage());
  diags.diagnose(requirement, diag::kind_declname_declared_here,
                 DescriptiveDeclKind::Protocol,
                 requirement->getName());
}

llvm::Optional<bool> ProtocolRequiresClassRequest::getCachedResult() const {
  auto decl = std::get<0>(getStorage());
  return decl->getCachedRequiresClass();
}

void ProtocolRequiresClassRequest::cacheResult(bool value) const {
  auto decl = std::get<0>(getStorage());
  decl->setCachedRequiresClass(value);
}

//----------------------------------------------------------------------------//
// existentialConformsToSelf computation.
//----------------------------------------------------------------------------//

void ExistentialConformsToSelfRequest::diagnoseCycle(DiagnosticEngine &diags) const {
  auto decl = std::get<0>(getStorage());
  diags.diagnose(decl, diag::circular_protocol_def, decl->getName());
}

void ExistentialConformsToSelfRequest::noteCycleStep(DiagnosticEngine &diags) const {
  auto requirement = std::get<0>(getStorage());
  diags.diagnose(requirement, diag::kind_declname_declared_here,
                 DescriptiveDeclKind::Protocol, requirement->getName());
}

llvm::Optional<bool> ExistentialConformsToSelfRequest::getCachedResult() const {
  auto decl = std::get<0>(getStorage());
  return decl->getCachedExistentialConformsToSelf();
}

void ExistentialConformsToSelfRequest::cacheResult(bool value) const {
  auto decl = std::get<0>(getStorage());
  decl->setCachedExistentialConformsToSelf(value);
}

//----------------------------------------------------------------------------//
// hasSelfOrAssociatedTypeRequirementsRequest computation.
//----------------------------------------------------------------------------//

void HasSelfOrAssociatedTypeRequirementsRequest::diagnoseCycle(
    DiagnosticEngine &diags) const {
  auto decl = std::get<0>(getStorage());
  diags.diagnose(decl, diag::circular_protocol_def, decl->getName());
}

void HasSelfOrAssociatedTypeRequirementsRequest::noteCycleStep(
    DiagnosticEngine &diags) const {
  auto requirement = std::get<0>(getStorage());
  diags.diagnose(requirement, diag::kind_declname_declared_here,
                 DescriptiveDeclKind::Protocol, requirement->getName());
}

llvm::Optional<bool>
HasSelfOrAssociatedTypeRequirementsRequest::getCachedResult() const {
  auto decl = std::get<0>(getStorage());
  return decl->getCachedHasSelfOrAssociatedTypeRequirements();
}

void HasSelfOrAssociatedTypeRequirementsRequest::cacheResult(bool value) const {
  auto decl = std::get<0>(getStorage());
  decl->setCachedHasSelfOrAssociatedTypeRequirements(value);
}

//----------------------------------------------------------------------------//
// isFinal computation.
//----------------------------------------------------------------------------//

llvm::Optional<bool> IsFinalRequest::getCachedResult() const {
  auto decl = std::get<0>(getStorage());
  if (decl->LazySemanticInfo.isFinalComputed)
    return decl->LazySemanticInfo.isFinal;

  return llvm::None;
}

void IsFinalRequest::cacheResult(bool value) const {
  auto decl = std::get<0>(getStorage());
  decl->LazySemanticInfo.isFinalComputed = true;
  decl->LazySemanticInfo.isFinal = value;

  // Add an attribute for printing
  if (value && !decl->getAttrs().hasAttribute<FinalAttr>())
    decl->getAttrs().add(new (decl->getASTContext()) FinalAttr(/*Implicit=*/true));
}

//----------------------------------------------------------------------------//
// isMoveOnly computation.
//----------------------------------------------------------------------------//

llvm::Optional<bool> IsMoveOnlyRequest::getCachedResult() const {
  auto decl = std::get<0>(getStorage());
  if (decl->LazySemanticInfo.isMoveOnlyComputed)
    return decl->LazySemanticInfo.isMoveOnly;

  return llvm::None;
}

void IsMoveOnlyRequest::cacheResult(bool value) const {
  auto decl = std::get<0>(getStorage());
  decl->LazySemanticInfo.isMoveOnlyComputed = true;
  decl->LazySemanticInfo.isMoveOnly = value;

  // Add an attribute for printing
  if (value && !decl->getAttrs().hasAttribute<MoveOnlyAttr>())
    decl->getAttrs().add(new (decl->getASTContext())
                             MoveOnlyAttr(/*Implicit=*/true));
}

//----------------------------------------------------------------------------//
// isDynamic computation.
//----------------------------------------------------------------------------//

llvm::Optional<bool> IsDynamicRequest::getCachedResult() const {
  auto decl = std::get<0>(getStorage());
  if (decl->LazySemanticInfo.isDynamicComputed)
    return decl->LazySemanticInfo.isDynamic;

  return llvm::None;
}

void IsDynamicRequest::cacheResult(bool value) const {
  auto decl = std::get<0>(getStorage());
  decl->setIsDynamic(value);

  // Add an attribute for printing
  if (value && !decl->getAttrs().hasAttribute<DynamicAttr>())
    decl->getAttrs().add(new (decl->getASTContext()) DynamicAttr(/*Implicit=*/true));
}

//----------------------------------------------------------------------------//
// RequirementSignatureRequest computation.
//----------------------------------------------------------------------------//

llvm::Optional<RequirementSignature>
RequirementSignatureRequest::getCachedResult() const {
  auto proto = std::get<0>(getStorage());
  if (proto->isRequirementSignatureComputed())
    return *proto->RequirementSig;

  return llvm::None;
}

void RequirementSignatureRequest::cacheResult(RequirementSignature value) const {
  auto proto = std::get<0>(getStorage());
  proto->setRequirementSignature(value);
}

//----------------------------------------------------------------------------//
// Requirement computation.
//----------------------------------------------------------------------------//

WhereClauseOwner::WhereClauseOwner(GenericContext *genCtx)
    : dc(genCtx),
      source(genCtx->getTrailingWhereClause()) {}

WhereClauseOwner::WhereClauseOwner(AssociatedTypeDecl *atd)
    : dc(atd->getInnermostDeclContext()),
      source(atd->getTrailingWhereClause()) {}

SourceLoc WhereClauseOwner::getLoc() const {
  if (auto genericParams = source.dyn_cast<GenericParamList *>()) {
    return genericParams->getWhereLoc();
  } else if (auto attr = source.dyn_cast<SpecializeAttr *>()) {
    return attr->getLocation();
  } else if (auto attr = source.dyn_cast<DifferentiableAttr *>()) {
    return attr->getLocation();
  } else if (auto where = source.dyn_cast<TrailingWhereClause *>()) {
    return where->getWhereLoc();
  }

  return SourceLoc();
}

void swift::simple_display(llvm::raw_ostream &out,
                           const WhereClauseOwner &owner) {
  if (owner.source.is<TrailingWhereClause *>()) {
    simple_display(out, owner.dc->getAsDecl());
  } else if (owner.source.is<SpecializeAttr *>()) {
    out << "@_specialize";
  } else if (owner.source.is<DifferentiableAttr *>()) {
    out << "@_differentiable";
  } else {
    out << "(SIL generic parameter list)";
  }
}

SourceLoc RequirementRequest::getNearestLoc() const {
  auto owner = std::get<0>(getStorage());
  return owner.getLoc();
}

void RequirementRequest::noteCycleStep(DiagnosticEngine &diags) const {
  // For now, the GSB does a better job of describing the exact structure of
  // the cycle.
  //
  // FIXME: We should consider merging the circularity handling the GSB does
  // into this request.  See rdar://55263708
}

MutableArrayRef<RequirementRepr> WhereClauseOwner::getRequirements() const {
  if (const auto genericParams = source.dyn_cast<GenericParamList *>()) {
    return genericParams->getRequirements();
  } else if (const auto attr = source.dyn_cast<SpecializeAttr *>()) {
    if (auto whereClause = attr->getTrailingWhereClause())
      return whereClause->getRequirements();
  } else if (const auto attr = source.dyn_cast<DifferentiableAttr *>()) {
    if (auto whereClause = attr->getWhereClause())
      return whereClause->getRequirements();
  } else if (const auto whereClause = source.get<TrailingWhereClause *>()) {
    return whereClause->getRequirements();
  }

  return { };
}

bool WhereClauseOwner::visitRequirements(
    TypeResolutionStage stage,
    llvm::function_ref<bool(Requirement, RequirementRepr *)> callback)
    const && {
  auto &evaluator = dc->getASTContext().evaluator;
  auto requirements = getRequirements();
  for (unsigned index : indices(requirements)) {
    // Resolve to a requirement.
    auto req = evaluator(RequirementRequest{*this, index, stage});
    if (req) {
      // Invoke the callback. If it returns true, we're done.
      if (callback(*req, &requirements[index]))
        return true;

      continue;
    }

    llvm::handleAllErrors(
        req.takeError(), [](const CyclicalRequestError<RequirementRequest> &E) {
          // cycle detected
        });
  }

  return false;
}

RequirementRepr &RequirementRequest::getRequirement() const {
  auto owner = std::get<0>(getStorage());
  auto index = std::get<1>(getStorage());
  return owner.getRequirements()[index];
}

bool RequirementRequest::isCached() const {
  return std::get<2>(getStorage()) == TypeResolutionStage::Interface;
}

//----------------------------------------------------------------------------//
// DefaultTypeRequest.
//----------------------------------------------------------------------------//

void swift::simple_display(llvm::raw_ostream &out,
                           const KnownProtocolKind kind) {
  out << getProtocolName(kind);
}

//----------------------------------------------------------------------------//
// DefaultTypeRequest caching.
//----------------------------------------------------------------------------//

llvm::Optional<Type> DefaultTypeRequest::getCachedResult() const {
  auto *DC = std::get<1>(getStorage());
  auto knownProtocolKind = std::get<0>(getStorage());
  const auto &cachedType = DC->getASTContext().getDefaultTypeRequestCache(
      DC->getParentSourceFile(), knownProtocolKind);
  return cachedType ? llvm::Optional<Type>(cachedType) : llvm::None;
}

void DefaultTypeRequest::cacheResult(Type value) const {
  auto *DC = std::get<1>(getStorage());
  auto knownProtocolKind = std::get<0>(getStorage());
  auto &cacheEntry = DC->getASTContext().getDefaultTypeRequestCache(
                         DC->getParentSourceFile(), knownProtocolKind);
  cacheEntry = value;
}

void swift::simple_display(
    llvm::raw_ostream &out, const PropertyWrapperTypeInfo &propertyWrapper) {
  out << "{ ";
  if (propertyWrapper.valueVar)
    out << propertyWrapper.valueVar->printRef();
  else
    out << "null";
  out << " }";
}

void swift::simple_display(
    llvm::raw_ostream &out,
    const PropertyWrapperInitializerInfo &initInfo) {
  out << "{";
  if (initInfo.hasInitFromWrappedValue())
    initInfo.getInitFromWrappedValue()->dump(out);
  if (initInfo.hasInitFromProjectedValue())
    initInfo.getInitFromProjectedValue()->dump(out);
  out << " }";
}

void swift::simple_display(
    llvm::raw_ostream &out,
    const PropertyWrapperAuxiliaryVariables &auxiliaryVars) {
  out << "{ ";
  if (auxiliaryVars.backingVar)
    auxiliaryVars.backingVar->dumpRef(out);
  out << " }";
}

void swift::simple_display(
  llvm::raw_ostream &out, const CtorInitializerKind initKind) {
  out << "{ ";
  switch (initKind) {
  case CtorInitializerKind::Designated:
    out << "designated"; break;
  case CtorInitializerKind::Convenience:
    out << "convenience"; break;
  case CtorInitializerKind::ConvenienceFactory:
    out << "convenience_factory"; break;
  case CtorInitializerKind::Factory:
    out << "factory"; break;
  }
  out << " }";
}

void swift::simple_display(llvm::raw_ostream &os, PropertyWrapperMutability m) {
  static const char *names[] =
    {"is nonmutating", "is mutating", "doesn't exist"};
  
  os << "getter " << names[m.Getter] << ", setter " << names[m.Setter];
}

void swift::simple_display(llvm::raw_ostream &out, PropertyWrapperLValueness l) {
  out << "is lvalue for get: {";
  simple_display(out, l.isLValueForGetAccess);
  out << "}, is lvalue for set: {";
  simple_display(out, l.isLValueForSetAccess);
  out << "}";
}

void swift::simple_display(llvm::raw_ostream &out,
                           ResilienceExpansion value) {
  switch (value) {
  case ResilienceExpansion::Minimal:
    out << "minimal";
    break;
  case ResilienceExpansion::Maximal:
    out << "maximal";
    break;
  }
}

void swift::simple_display(llvm::raw_ostream &out,
                           FragileFunctionKind value) {
  switch (value.kind) {
  case FragileFunctionKind::Transparent:
    out << "transparent";
    break;
  case FragileFunctionKind::Inlinable:
    out << "inlinable";
    break;
  case FragileFunctionKind::AlwaysEmitIntoClient:
    out << "alwaysEmitIntoClient";
    break;
  case FragileFunctionKind::DefaultArgument:
    out << "defaultArgument";
    break;
  case FragileFunctionKind::PropertyInitializer:
    out << "propertyInitializer";
    break;
  case FragileFunctionKind::BackDeploy:
    out << "backDeploy";
    break;
  case FragileFunctionKind::None:
    out << "none";
    break;
  }

  out << ", allowUsableFromInline: "
      << (value.allowUsableFromInline ? "true" : "false");
}

//----------------------------------------------------------------------------//
// SelfAccessKindRequest computation.
//----------------------------------------------------------------------------//

llvm::Optional<SelfAccessKind> SelfAccessKindRequest::getCachedResult() const {
  auto *funcDecl = std::get<0>(getStorage());
  return funcDecl->getCachedSelfAccessKind();
}

void SelfAccessKindRequest::cacheResult(SelfAccessKind value) const {
  auto *funcDecl = std::get<0>(getStorage());
  funcDecl->setSelfAccessKind(value);
}

//----------------------------------------------------------------------------//
// IsGetterMutatingRequest computation.
//----------------------------------------------------------------------------//

llvm::Optional<bool> IsGetterMutatingRequest::getCachedResult() const {
  auto *storage = std::get<0>(getStorage());
  if (storage->LazySemanticInfo.IsGetterMutatingComputed)
    return storage->LazySemanticInfo.IsGetterMutating;
  return llvm::None;
}

void IsGetterMutatingRequest::cacheResult(bool value) const {
  auto *storage = std::get<0>(getStorage());
  storage->setIsGetterMutating(value);
}

//----------------------------------------------------------------------------//
// IsSetterMutatingRequest computation.
//----------------------------------------------------------------------------//

llvm::Optional<bool> IsSetterMutatingRequest::getCachedResult() const {
  auto *storage = std::get<0>(getStorage());
  if (storage->LazySemanticInfo.IsSetterMutatingComputed)
    return storage->LazySemanticInfo.IsSetterMutating;
  return llvm::None;
}

void IsSetterMutatingRequest::cacheResult(bool value) const {
  auto *storage = std::get<0>(getStorage());
  storage->setIsSetterMutating(value);
}

//----------------------------------------------------------------------------//
// OpaqueReadOwnershipRequest computation.
//----------------------------------------------------------------------------//

llvm::Optional<OpaqueReadOwnership>
OpaqueReadOwnershipRequest::getCachedResult() const {
  auto *storage = std::get<0>(getStorage());
  if (storage->LazySemanticInfo.OpaqueReadOwnershipComputed)
    return OpaqueReadOwnership(storage->LazySemanticInfo.OpaqueReadOwnership);
  return llvm::None;
}

void OpaqueReadOwnershipRequest::cacheResult(OpaqueReadOwnership value) const {
  auto *storage = std::get<0>(getStorage());
  storage->setOpaqueReadOwnership(value);
}

//----------------------------------------------------------------------------//
// StorageImplInfoRequest computation.
//----------------------------------------------------------------------------//

llvm::Optional<StorageImplInfo>
StorageImplInfoRequest::getCachedResult() const {
  auto *storage = std::get<0>(getStorage());
  if (storage->LazySemanticInfo.ImplInfoComputed)
    return storage->ImplInfo;
  return llvm::None;
}

void StorageImplInfoRequest::cacheResult(StorageImplInfo value) const {
  auto *storage = std::get<0>(getStorage());
  storage->cacheImplInfo(value);
}

//----------------------------------------------------------------------------//
// RequiresOpaqueAccessorsRequest computation.
//----------------------------------------------------------------------------//

llvm::Optional<bool> RequiresOpaqueAccessorsRequest::getCachedResult() const {
  auto *storage = std::get<0>(getStorage());
  if (storage->LazySemanticInfo.RequiresOpaqueAccessorsComputed)
    return storage->LazySemanticInfo.RequiresOpaqueAccessors;
  return llvm::None;
}

void RequiresOpaqueAccessorsRequest::cacheResult(bool value) const {
  auto *storage = std::get<0>(getStorage());
  storage->LazySemanticInfo.RequiresOpaqueAccessorsComputed = 1;
  storage->LazySemanticInfo.RequiresOpaqueAccessors = value;
}

//----------------------------------------------------------------------------//
// RequiresOpaqueModifyCoroutineRequest computation.
//----------------------------------------------------------------------------//

llvm::Optional<bool>
RequiresOpaqueModifyCoroutineRequest::getCachedResult() const {
  auto *storage = std::get<0>(getStorage());
  if (storage->LazySemanticInfo.RequiresOpaqueModifyCoroutineComputed)
    return storage->LazySemanticInfo.RequiresOpaqueModifyCoroutine;
  return llvm::None;
}

void RequiresOpaqueModifyCoroutineRequest::cacheResult(bool value) const {
  auto *storage = std::get<0>(getStorage());
  storage->LazySemanticInfo.RequiresOpaqueModifyCoroutineComputed = 1;
  storage->LazySemanticInfo.RequiresOpaqueModifyCoroutine = value;
}

//----------------------------------------------------------------------------//
// IsAccessorTransparentRequest computation.
//----------------------------------------------------------------------------//

llvm::Optional<bool> IsAccessorTransparentRequest::getCachedResult() const {
  auto *accessor = std::get<0>(getStorage());
  return accessor->getCachedIsTransparent();
}

void IsAccessorTransparentRequest::cacheResult(bool value) const {
  auto *accessor = std::get<0>(getStorage());
  accessor->setIsTransparent(value);

  // For interface printing, API diff, etc.
  if (value) {
    auto &attrs = accessor->getAttrs();
    if (!attrs.hasAttribute<TransparentAttr>()) {
      auto &ctx = accessor->getASTContext();
      attrs.add(new (ctx) TransparentAttr(/*IsImplicit=*/true));
    }
  }
}

//----------------------------------------------------------------------------//
// SynthesizeAccessorRequest computation.
//----------------------------------------------------------------------------//

llvm::Optional<AccessorDecl *>
SynthesizeAccessorRequest::getCachedResult() const {
  auto *storage = std::get<0>(getStorage());
  auto kind = std::get<1>(getStorage());
  auto *accessor = storage->getAccessor(kind);
  if (accessor)
    return accessor;
  return llvm::None;
}

void SynthesizeAccessorRequest::cacheResult(AccessorDecl *accessor) const {
  auto *storage = std::get<0>(getStorage());
  auto kind = std::get<1>(getStorage());

  storage->setSynthesizedAccessor(kind, accessor);
}

//----------------------------------------------------------------------------//
// IsImplicitlyUnwrappedOptionalRequest computation.
//----------------------------------------------------------------------------//

llvm::Optional<bool>
IsImplicitlyUnwrappedOptionalRequest::getCachedResult() const {
  auto *decl = std::get<0>(getStorage());
  if (decl->LazySemanticInfo.isIUOComputed)
    return decl->LazySemanticInfo.isIUO;
  return llvm::None;
}

void IsImplicitlyUnwrappedOptionalRequest::cacheResult(bool value) const {
  auto *decl = std::get<0>(getStorage());
  decl->setImplicitlyUnwrappedOptional(value);
}

//----------------------------------------------------------------------------//
// GenericSignatureRequest computation.
//----------------------------------------------------------------------------//

llvm::Optional<GenericSignature>
GenericSignatureRequest::getCachedResult() const {
  auto *GC = std::get<0>(getStorage());
  if (GC->GenericSigAndBit.getInt()) {
    return GC->GenericSigAndBit.getPointer();
  }
  return llvm::None;
}

void GenericSignatureRequest::cacheResult(GenericSignature value) const {
  auto *GC = std::get<0>(getStorage());
  GC->GenericSigAndBit.setPointerAndInt(value, true);
}

void GenericSignatureRequest::diagnoseCycle(DiagnosticEngine &diags) const {
  auto *GC = std::get<0>(getStorage());
  auto *D = GC->getAsDecl();

  if (auto *VD = dyn_cast<ValueDecl>(D)) {
    VD->diagnose(diag::recursive_generic_signature,
                 VD->getDescriptiveKind(), VD->getBaseName());
  } else {
    auto *ED = cast<ExtensionDecl>(D);
    auto *NTD = ED->getExtendedNominal();

    ED->diagnose(diag::recursive_generic_signature_extension,
                 NTD->getDescriptiveKind(), NTD->getName());
  }
}

//----------------------------------------------------------------------------//
// InferredGenericSignatureRequest computation.
//----------------------------------------------------------------------------//

void InferredGenericSignatureRequest::noteCycleStep(DiagnosticEngine &d) const {
  // For now, the GSB does a better job of describing the exact structure of
  // the cycle.
  //
  // FIXME: We should consider merging the circularity handling the GSB does
  // into this request.  See rdar://55263708
}

//----------------------------------------------------------------------------//
// UnderlyingTypeRequest computation.
//----------------------------------------------------------------------------//

llvm::Optional<Type> UnderlyingTypeRequest::getCachedResult() const {
  auto *typeAlias = std::get<0>(getStorage());
  if (auto type = typeAlias->UnderlyingTy.getType())
    return type;
  return llvm::None;
}

void UnderlyingTypeRequest::cacheResult(Type value) const {
  auto *typeAlias = std::get<0>(getStorage());
  typeAlias->UnderlyingTy.setType(value);
}

void UnderlyingTypeRequest::diagnoseCycle(DiagnosticEngine &diags) const {
  auto aliasDecl = std::get<0>(getStorage());
  diags.diagnose(aliasDecl, diag::recursive_decl_reference,
                 aliasDecl->getDescriptiveKind(),
                 aliasDecl->getName());
}

//----------------------------------------------------------------------------//
// StructuralTypeRequest computation.
//----------------------------------------------------------------------------//

void StructuralTypeRequest::diagnoseCycle(DiagnosticEngine &diags) const {
  auto aliasDecl = std::get<0>(getStorage());
  diags.diagnose(aliasDecl, diag::recursive_decl_reference,
                 aliasDecl->getDescriptiveKind(),
                 aliasDecl->getName());
}

//----------------------------------------------------------------------------//
// EnumRawValuesRequest computation.
//----------------------------------------------------------------------------//

bool EnumRawValuesRequest::isCached() const {
  return std::get<1>(getStorage()) == TypeResolutionStage::Interface;
}

llvm::Optional<evaluator::SideEffect>
EnumRawValuesRequest::getCachedResult() const {
  auto *ED = std::get<0>(getStorage());
  if (ED->SemanticFlags.contains(EnumDecl::HasFixedRawValuesAndTypes))
    return std::make_tuple<>();
  return llvm::None;
}

void EnumRawValuesRequest::cacheResult(evaluator::SideEffect) const {
  auto *ED = std::get<0>(getStorage());
  ED->SemanticFlags |= OptionSet<EnumDecl::SemanticInfoFlags>{
      EnumDecl::HasFixedRawValues | EnumDecl::HasFixedRawValuesAndTypes};
}

void EnumRawValuesRequest::diagnoseCycle(DiagnosticEngine &diags) const {
  // This request computes the raw type, and so participates in cycles involving
  // it. For now, the raw type provides a rich enough circularity diagnostic
  // that we can silence ourselves.
}

void EnumRawValuesRequest::noteCycleStep(DiagnosticEngine &diags) const {
  
}

//----------------------------------------------------------------------------//
// IsStaticRequest computation.
//----------------------------------------------------------------------------//

llvm::Optional<bool> IsStaticRequest::getCachedResult() const {
  auto *FD = std::get<0>(getStorage());
  return FD->getCachedIsStatic();
}

void IsStaticRequest::cacheResult(bool result) const {
  auto *FD = std::get<0>(getStorage());
  FD->setStatic(result);
}

//----------------------------------------------------------------------------//
// NeedsNewVTableEntryRequest computation.
//----------------------------------------------------------------------------//

llvm::Optional<bool> NeedsNewVTableEntryRequest::getCachedResult() const {
  auto *decl = std::get<0>(getStorage());
  if (decl->LazySemanticInfo.NeedsNewVTableEntryComputed)
    return decl->LazySemanticInfo.NeedsNewVTableEntry;
  return llvm::None;
}

void NeedsNewVTableEntryRequest::cacheResult(bool value) const {
  auto *decl = std::get<0>(getStorage());
  decl->LazySemanticInfo.NeedsNewVTableEntryComputed = true;
  decl->LazySemanticInfo.NeedsNewVTableEntry = value;
}

//----------------------------------------------------------------------------//
// ParamSpecifierRequest computation.
//----------------------------------------------------------------------------//

llvm::Optional<ParamSpecifier> ParamSpecifierRequest::getCachedResult() const {
  auto *decl = std::get<0>(getStorage());
  return decl->getCachedSpecifier();
}

void ParamSpecifierRequest::cacheResult(ParamSpecifier specifier) const {
  auto *decl = std::get<0>(getStorage());
  decl->setSpecifier(specifier);
}

//----------------------------------------------------------------------------//
// ResultTypeRequest computation.
//----------------------------------------------------------------------------//

llvm::Optional<Type> ResultTypeRequest::getCachedResult() const {
  Type type;
  auto *const decl = std::get<0>(getStorage());
  if (const auto *const funcDecl = dyn_cast<FuncDecl>(decl)) {
    type = funcDecl->FnRetType.getType();
  } else if (auto subscript = dyn_cast<SubscriptDecl>(decl)) {
    type = subscript->ElementTy.getType();
  } else {
    type = cast<MacroDecl>(decl)->resultType.getType();
  }

  if (type.isNull())
    return llvm::None;

  return type;
}

void ResultTypeRequest::cacheResult(Type type) const {
  auto *const decl = std::get<0>(getStorage());
  if (auto *const funcDecl = dyn_cast<FuncDecl>(decl)) {
    funcDecl->FnRetType.setType(type);
  } else if (auto subscript = dyn_cast<SubscriptDecl>(decl)) {
    subscript->ElementTy.setType(type);
  } else {
    cast<MacroDecl>(decl)->resultType.setType(type);
  }
}

//----------------------------------------------------------------------------//
// PatternBindingEntryRequest computation.
//----------------------------------------------------------------------------//

llvm::Optional<const PatternBindingEntry *>
PatternBindingEntryRequest::getCachedResult() const {
  auto *PBD = std::get<0>(getStorage());
  auto idx = std::get<1>(getStorage());
  if (!PBD->getPatternList()[idx].isFullyValidated()) {
    return llvm::None;
  }
  return &PBD->getPatternList()[idx];
}

void PatternBindingEntryRequest::cacheResult(
    const PatternBindingEntry *value) const {
  auto *PBD = std::get<0>(getStorage());
  auto idx = std::get<1>(getStorage());
  PBD->getMutablePatternList()[idx].setFullyValidated();
}

//----------------------------------------------------------------------------//
// NamingPatternRequest computation.
//----------------------------------------------------------------------------//

llvm::Optional<NamedPattern *> NamingPatternRequest::getCachedResult() const {
  auto *VD = std::get<0>(getStorage());
  if (auto *Pat = VD->NamingPattern) {
    return Pat;
  }
  return llvm::None;
}

void NamingPatternRequest::cacheResult(NamedPattern *value) const {
  auto *VD = std::get<0>(getStorage());
  VD->NamingPattern = value;
}

//----------------------------------------------------------------------------//
// ExprPatternMatchRequest caching.
//----------------------------------------------------------------------------//

llvm::Optional<ExprPatternMatchResult>
ExprPatternMatchRequest::getCachedResult() const {
  auto *EP = std::get<0>(getStorage());
  if (!EP->MatchVar)
    return llvm::None;

  return ExprPatternMatchResult(EP->MatchVar, EP->MatchExpr);
}

void ExprPatternMatchRequest::cacheResult(ExprPatternMatchResult result) const {
  auto *EP = std::get<0>(getStorage());
  EP->MatchVar = result.getMatchVar();
  EP->MatchExpr = result.getMatchExpr();
}

//----------------------------------------------------------------------------//
// InterfaceTypeRequest computation.
//----------------------------------------------------------------------------//

llvm::Optional<Type> InterfaceTypeRequest::getCachedResult() const {
  auto *decl = std::get<0>(getStorage());
  if (auto Ty = decl->TypeAndAccess.getPointer()) {
    return Ty;
  }
  return llvm::None;
}

void InterfaceTypeRequest::cacheResult(Type type) const {
  auto *decl = std::get<0>(getStorage());
  if (type) {
    assert(!type->hasTypeVariable() && "Type variable in interface type");
    assert(!type->is<InOutType>() && "Interface type must be materializable");
    assert(!type->hasArchetype() && "Archetype in interface type");
  }
  decl->TypeAndAccess.setPointer(type);
}

//----------------------------------------------------------------------------//
// ValidatePrecedenceGroupRequest computation.
//----------------------------------------------------------------------------//

SourceLoc ValidatePrecedenceGroupRequest::getNearestLoc() const {
  auto &desc = std::get<0>(getStorage());
  return desc.getLoc();
}

void ValidatePrecedenceGroupRequest::diagnoseCycle(
    DiagnosticEngine &diags) const {
  auto &desc = std::get<0>(getStorage());
  if (auto pathDir = desc.pathDirection) {
    diags.diagnose(desc.nameLoc, diag::precedence_group_cycle, (bool)*pathDir);
  } else {
    diags.diagnose(desc.nameLoc, diag::circular_reference);
  }
}

void ValidatePrecedenceGroupRequest::noteCycleStep(
    DiagnosticEngine &diag) const {
  auto &desc = std::get<0>(getStorage());
  diag.diagnose(desc.nameLoc,
                 diag::circular_reference_through_precedence_group, desc.ident);
}

SourceLoc PrecedenceGroupDescriptor::getLoc() const {
  return nameLoc;
}

void swift::simple_display(llvm::raw_ostream &out,
                           const PrecedenceGroupDescriptor &desc) {
  out << "precedence group " << desc.ident << " at ";
  desc.nameLoc.print(out, desc.dc->getASTContext().SourceMgr);
}

//----------------------------------------------------------------------------//
// InheritsSuperclassInitializersRequest computation.
//----------------------------------------------------------------------------//

llvm::Optional<bool>
InheritsSuperclassInitializersRequest::getCachedResult() const {
  auto *decl = std::get<0>(getStorage());
  return decl->getCachedInheritsSuperclassInitializers();
}

void InheritsSuperclassInitializersRequest::cacheResult(bool value) const {
  auto *decl = std::get<0>(getStorage());
  decl->setInheritsSuperclassInitializers(value);
}

//----------------------------------------------------------------------------//
// ResolveImplicitMemberRequest computation.
//----------------------------------------------------------------------------//

void swift::simple_display(llvm::raw_ostream &out,
                           ImplicitMemberAction action) {
  switch (action) {
  case ImplicitMemberAction::ResolveImplicitInit:
    out << "resolve implicit initializer";
    break;
  case ImplicitMemberAction::ResolveCodingKeys:
    out << "resolve CodingKeys";
    break;
  case ImplicitMemberAction::ResolveEncodable:
    out << "resolve Encodable.encode(to:)";
    break;
  case ImplicitMemberAction::ResolveDecodable:
    out << "resolve Decodable.init(from:)";
    break;
  case ImplicitMemberAction::ResolveDistributedActor:
    out << "resolve DistributedActor";
    break;
  case ImplicitMemberAction::ResolveDistributedActorID:
    out << "resolve DistributedActor.id";
    break;
  case ImplicitMemberAction::ResolveDistributedActorSystem:
    out << "resolve DistributedActor.actorSystem";
    break;
  }
}

//----------------------------------------------------------------------------//
// TypeWitnessRequest computation.
//----------------------------------------------------------------------------//

llvm::Optional<TypeWitnessAndDecl> TypeWitnessRequest::getCachedResult() const {
  auto *conformance = std::get<0>(getStorage());
  auto *requirement = std::get<1>(getStorage());
  if (conformance->TypeWitnesses.count(requirement) == 0) {
    return llvm::None;
  }
  return conformance->TypeWitnesses[requirement];
}

void TypeWitnessRequest::cacheResult(TypeWitnessAndDecl typeWitAndDecl) const {
  // FIXME: Refactor this to be the thing that warms the cache.
}

//----------------------------------------------------------------------------//
// WitnessRequest computation.
//----------------------------------------------------------------------------//

llvm::Optional<Witness> ValueWitnessRequest::getCachedResult() const {
  auto *conformance = std::get<0>(getStorage());
  auto *requirement = std::get<1>(getStorage());
  if (conformance->Mapping.count(requirement) == 0) {
    return llvm::None;
  }
  return conformance->Mapping[requirement];
}

void ValueWitnessRequest::cacheResult(Witness type) const {
  // FIXME: Refactor this to be the thing that warms the cache.
}

//----------------------------------------------------------------------------//
// PreCheckResultBuilderRequest computation.
//----------------------------------------------------------------------------//

void swift::simple_display(llvm::raw_ostream &out,
                           ResultBuilderBodyPreCheck value) {
  switch (value) {
  case ResultBuilderBodyPreCheck::Okay:
    out << "okay";
    break;
  case ResultBuilderBodyPreCheck::HasReturnStmt:
    out << "has return statement";
    break;
  case ResultBuilderBodyPreCheck::Error:
    out << "error";
    break;
  }
}

//----------------------------------------------------------------------------//
// HasCircularInheritedProtocolsRequest computation.
//----------------------------------------------------------------------------//

void HasCircularInheritedProtocolsRequest::diagnoseCycle(
    DiagnosticEngine &diags) const {
  auto *decl = std::get<0>(getStorage());
  diags.diagnose(decl, diag::circular_protocol_def, decl->getName());
}

void HasCircularInheritedProtocolsRequest::noteCycleStep(
    DiagnosticEngine &diags) const {
  auto *decl = std::get<0>(getStorage());
  diags.diagnose(decl, diag::kind_declname_declared_here,
                 decl->getDescriptiveKind(), decl->getName());
}

//----------------------------------------------------------------------------//
// HasCircularRawValueRequest computation.
//----------------------------------------------------------------------------//

void HasCircularRawValueRequest::diagnoseCycle(DiagnosticEngine &diags) const {
  auto *decl = std::get<0>(getStorage());
  diags.diagnose(decl, diag::circular_enum_inheritance, decl->getName());
}

void HasCircularRawValueRequest::noteCycleStep(DiagnosticEngine &diags) const {
  auto *decl = std::get<0>(getStorage());
  diags.diagnose(decl, diag::kind_declname_declared_here,
                 decl->getDescriptiveKind(), decl->getName());
}

//----------------------------------------------------------------------------//
// DefaultArgumentInitContextRequest computation.
//----------------------------------------------------------------------------//

llvm::Optional<Initializer *>
DefaultArgumentInitContextRequest::getCachedResult() const {
  auto *param = std::get<0>(getStorage());
  return param->getCachedDefaultArgumentInitContext();
}

void DefaultArgumentInitContextRequest::cacheResult(Initializer *init) const {
  auto *param = std::get<0>(getStorage());
  param->setDefaultArgumentInitContext(init);
}

//----------------------------------------------------------------------------//
// DefaultArgumentExprRequest computation.
//----------------------------------------------------------------------------//

llvm::Optional<Expr *> DefaultArgumentExprRequest::getCachedResult() const {
  auto *param = std::get<0>(getStorage());
  auto *defaultInfo = param->DefaultValueAndFlags.getPointer();
  if (!defaultInfo)
    return llvm::None;

  if (!defaultInfo->InitContextAndIsTypeChecked.getInt())
    return llvm::None;

  return defaultInfo->DefaultArg.get<Expr *>();
}

void DefaultArgumentExprRequest::cacheResult(Expr *expr) const {
  auto *param = std::get<0>(getStorage());
  param->setDefaultExpr(expr, /*isTypeChecked*/ true);
}

//----------------------------------------------------------------------------//
// DefaultArgumentTypeRequest computation.
//----------------------------------------------------------------------------//

llvm::Optional<Type> DefaultArgumentTypeRequest::getCachedResult() const {
  auto *param = std::get<0>(getStorage());
  auto *defaultInfo = param->DefaultValueAndFlags.getPointer();
  if (!defaultInfo)
    return llvm::None;

  return defaultInfo->ExprType ? llvm::Optional<Type>(defaultInfo->ExprType)
                               : llvm::None;
}

void DefaultArgumentTypeRequest::cacheResult(Type type) const {
  auto *param = std::get<0>(getStorage());
  param->setDefaultExprType(type);
}

//----------------------------------------------------------------------------//
// CallerSideDefaultArgExprRequest computation.
//----------------------------------------------------------------------------//

llvm::Optional<Expr *>
CallerSideDefaultArgExprRequest::getCachedResult() const {
  auto *defaultExpr = std::get<0>(getStorage());
  auto storage = defaultExpr->ContextOrCallerSideExpr;
  assert(!storage.isNull());

  if (auto *expr = storage.dyn_cast<Expr *>())
    return expr;

  return llvm::None;
}

void CallerSideDefaultArgExprRequest::cacheResult(Expr *expr) const {
  auto *defaultExpr = std::get<0>(getStorage());
  defaultExpr->ContextOrCallerSideExpr = expr;
}

//----------------------------------------------------------------------------//
// DifferentiableAttributeTypeCheckRequest computation.
//----------------------------------------------------------------------------//

llvm::Optional<IndexSubset *>
DifferentiableAttributeTypeCheckRequest::getCachedResult() const {
  auto *attr = std::get<0>(getStorage());
  if (attr->hasBeenTypeChecked())
    return attr->ParameterIndicesAndBit.getPointer();
  return llvm::None;
}

void DifferentiableAttributeTypeCheckRequest::cacheResult(
    IndexSubset *parameterIndices) const {
  auto *attr = std::get<0>(getStorage());
  attr->ParameterIndicesAndBit.setPointerAndInt(parameterIndices, true);
}

//----------------------------------------------------------------------------//
// CheckRedeclarationRequest computation.
//----------------------------------------------------------------------------//

llvm::Optional<evaluator::SideEffect>
CheckRedeclarationRequest::getCachedResult() const {
  if (!std::get<0>(getStorage())->alreadyCheckedRedeclaration())
    return llvm::None;
  return std::make_tuple<>();
}

void CheckRedeclarationRequest::cacheResult(evaluator::SideEffect) const {
  std::get<0>(getStorage())->setCheckedRedeclaration();
}

evaluator::DependencySource CheckRedeclarationRequest::readDependencySource(
    const evaluator::DependencyRecorder &eval) const {
  auto *currentDC = std::get<0>(getStorage())->getDeclContext();
  return currentDC->getParentSourceFile();
}

void CheckRedeclarationRequest::writeDependencySink(
    evaluator::DependencyCollector &tracker, evaluator::SideEffect) const {
  auto *current = std::get<0>(getStorage());
  if (!current->hasName())
    return;

  DeclContext *currentDC = current->getDeclContext();
  SourceFile *currentFile = currentDC->getParentSourceFile();
  if (!currentFile || currentDC->isLocalContext())
    return;

  if (currentDC->isTypeContext()) {
    if (auto nominal = std::get<1>(getStorage())) {
      tracker.addUsedMember(nominal, current->getBaseName());
    }
  } else {
    tracker.addTopLevelName(current->getBaseName());
  }
}

//----------------------------------------------------------------------------//
// LookupAllConformancesInContextRequest computation.
//----------------------------------------------------------------------------//

void LookupAllConformancesInContextRequest::writeDependencySink(
    evaluator::DependencyCollector &tracker,
    const ProtocolConformanceLookupResult &conformances) const {
  for (auto conformance : conformances) {
    tracker.addPotentialMember(conformance->getProtocol());
  }
}

//----------------------------------------------------------------------------//
// ResolveTypeEraserTypeRequest computation.
//----------------------------------------------------------------------------//

llvm::Optional<Type> ResolveTypeEraserTypeRequest::getCachedResult() const {
  auto *TyExpr = std::get<1>(getStorage())->TypeEraserExpr;
  if (!TyExpr || !TyExpr->getType()) {
    return llvm::None;
  }
  return TyExpr->getInstanceType();
}

void ResolveTypeEraserTypeRequest::cacheResult(Type value) const {
  assert(value && "Resolved type erasure type to null type!");
  auto *attr = std::get<1>(getStorage());
  if (attr->TypeEraserExpr) {
    attr->TypeEraserExpr->setType(MetatypeType::get(value));
  } else {
    attr->TypeEraserExpr = TypeExpr::createImplicit(value,
                                                    value->getASTContext());
  }
}

//----------------------------------------------------------------------------//
// TypeCheckSourceFileRequest computation.
//----------------------------------------------------------------------------//

evaluator::DependencySource TypeCheckSourceFileRequest::readDependencySource(
    const evaluator::DependencyRecorder &e) const {
  return std::get<0>(getStorage());
}

llvm::Optional<evaluator::SideEffect>
TypeCheckSourceFileRequest::getCachedResult() const {
  auto *SF = std::get<0>(getStorage());
  if (SF->ASTStage == SourceFile::TypeChecked)
    return std::make_tuple<>();

  return llvm::None;
}

void TypeCheckSourceFileRequest::cacheResult(evaluator::SideEffect) const {
  auto *SF = std::get<0>(getStorage());

  // Verify that we've checked types correctly.
  SF->ASTStage = SourceFile::TypeChecked;

  {
    auto &Ctx = SF->getASTContext();
    FrontendStatsTracer tracer(Ctx.Stats, "AST verification");
    // Verify the SourceFile.
    swift::verify(*SF);
  }
}

//----------------------------------------------------------------------------//
// TypeCheckFunctionBodyRequest computation.
//----------------------------------------------------------------------------//

llvm::Optional<BraceStmt *>
TypeCheckFunctionBodyRequest::getCachedResult() const {
  using BodyKind = AbstractFunctionDecl::BodyKind;
  auto *afd = std::get<0>(getStorage());
  switch (afd->getBodyKind()) {
  case BodyKind::Deserialized:
  case BodyKind::SILSynthesize:
  case BodyKind::None:
  case BodyKind::Skipped:
    // These cases don't have any body available.
    return nullptr;

  case BodyKind::TypeChecked:
    return afd->BodyAndFP.getBody();

  case BodyKind::Synthesize:
  case BodyKind::Parsed:
  case BodyKind::Unparsed:
    return llvm::None;
  }
  llvm_unreachable("Unhandled BodyKind in switch");
}

void TypeCheckFunctionBodyRequest::cacheResult(BraceStmt *body) const {
  auto *afd = std::get<0>(getStorage());
  afd->setBody(body, AbstractFunctionDecl::BodyKind::TypeChecked);
}

evaluator::DependencySource
TypeCheckFunctionBodyRequest::readDependencySource(
    const evaluator::DependencyRecorder &e) const {
  return std::get<0>(getStorage())->getParentSourceFile();
}

//----------------------------------------------------------------------------//
// ModuleImplicitImportsRequest computation.
//----------------------------------------------------------------------------//

void swift::simple_display(llvm::raw_ostream &out,
                           const ImportedModule &module) {
  out << "import of ";
  if (!module.accessPath.empty()) {
    module.accessPath.print(out);
    out << " in ";
  }
  simple_display(out, module.importedModule);
}

void swift::simple_display(llvm::raw_ostream &out,
                           const UnloadedImportedModule &module) {
  out << "import of ";
  if (!module.getAccessPath().empty()) {
    module.getAccessPath().print(out);
    out << " in ";
  }
  out << "unloaded ";
  module.getModulePath().print(out);
}

void swift::simple_display(llvm::raw_ostream &out,
                           const AttributedImport<std::tuple<>> &import) {
  out << " [";

  if (import.options.contains(ImportFlags::Exported))
    out << " exported";
  if (import.options.contains(ImportFlags::Testable))
    out << " testable";
  if (import.options.contains(ImportFlags::ImplementationOnly))
    out << " implementation-only";
  if (import.options.contains(ImportFlags::PrivateImport))
    out << " private(" << import.sourceFileArg << ")";

  if (import.options.contains(ImportFlags::SPIAccessControl)) {
    out << " spi(";
    llvm::interleaveComma(import.spiGroups, out, [&out](Identifier name) {
                                                   simple_display(out, name);
                                                 });
    out << ")";
  }

  if (import.options.contains(ImportFlags::Preconcurrency))
    out << " preconcurrency";

  if (import.options.contains(ImportFlags::WeakLinked))
    out << " weak-linked";

  out << " ]";
}

void swift::simple_display(llvm::raw_ostream &out,
                           const ImplicitImportList &importList) {
  llvm::interleaveComma(importList.imports, out,
                        [&](const auto &import) {
                          simple_display(out, import);
                        });
  if (!importList.imports.empty() && !importList.unloadedImports.empty())
    out << ", ";
  llvm::interleaveComma(importList.unloadedImports, out,
                        [&](const auto &import) {
                          simple_display(out, import);
                        });
}

//----------------------------------------------------------------------------//
// ResolveTypeRequest computation.
//----------------------------------------------------------------------------//

void ResolveTypeRequest::noteCycleStep(DiagnosticEngine &diags) const {
  auto *repr = std::get<1>(getStorage());
  diags.diagnose(repr->getLoc(), diag::circular_type_resolution_note, repr);
}

void swift::simple_display(llvm::raw_ostream &out,
                           const TypeResolution *resolution) {
  out << "while resolving type ";
}

SourceLoc swift::extractNearestSourceLoc(const TypeRepr *repr) {
  if (!repr)
    return SourceLoc();
  return repr->getLoc();
}

//----------------------------------------------------------------------------//
// CustomAttrTypeRequest computation.
//----------------------------------------------------------------------------//

void swift::simple_display(llvm::raw_ostream &out, CustomAttrTypeKind value) {
  switch (value) {
  case CustomAttrTypeKind::NonGeneric:
    out << "non-generic";
    return;

  case CustomAttrTypeKind::PropertyWrapper:
    out << "property-wrapper";
    return;

  case CustomAttrTypeKind::GlobalActor:
    out << "global-actor";
    return;

  case CustomAttrTypeKind::RuntimeMetadata:
    out << "runtime-metadata";
    return;
  }

  llvm_unreachable("bad kind");
}

llvm::Optional<Type> CustomAttrTypeRequest::getCachedResult() const {
  auto *attr = std::get<0>(getStorage());
  if (auto ty = attr->getType()) {
    return ty;
  }
  return llvm::None;
}

void CustomAttrTypeRequest::cacheResult(Type value) const {
  auto *attr = std::get<0>(getStorage());
  attr->setType(value);
}

SourceLoc MacroDefinitionRequest::getNearestLoc() const {
  auto &desc = std::get<0>(getStorage());
  return desc->getLoc();
}

bool ActorIsolation::requiresSubstitution() const {
  switch (kind) {
  case ActorInstance:
  case Independent:
  case Unspecified:
    return false;

  case GlobalActor:
  case GlobalActorUnsafe:
    return getGlobalActor()->hasTypeParameter();
  }
  llvm_unreachable("unhandled actor isolation kind!");
}

ActorIsolation ActorIsolation::subst(SubstitutionMap subs) const {
  switch (kind) {
  case ActorInstance:
  case Independent:
  case Unspecified:
    return *this;

  case GlobalActor:
  case GlobalActorUnsafe:
    return forGlobalActor(
        getGlobalActor().subst(subs), kind == GlobalActorUnsafe)
              .withPreconcurrency(preconcurrency());
  }
  llvm_unreachable("unhandled actor isolation kind!");
}

void swift::simple_display(
    llvm::raw_ostream &out, const ActorIsolation &state) {
  switch (state) {
    case ActorIsolation::ActorInstance:
      out << "actor-isolated to instance of ";
      if (state.isDistributedActor()) {
        out << "distributed ";
      }
      out << "actor " << state.getActor()->getName();
      break;

    case ActorIsolation::Independent:
      out << "actor-independent";
      break;

    case ActorIsolation::Unspecified:
      out << "unspecified actor isolation";
      break;

    case ActorIsolation::GlobalActor:
    case ActorIsolation::GlobalActorUnsafe:
      out << "actor-isolated to global actor "
          << state.getGlobalActor().getString();

      if (state == ActorIsolation::GlobalActorUnsafe)
        out << "(unsafe)";
      break;
  }
}

bool swift::areTypesEqual(Type type1, Type type2) {
  if (!type1 || !type2)
    return !type1 && !type2;

  return type1->isEqual(type2);
}

void swift::simple_display(
    llvm::raw_ostream &out, BodyInitKind initKind) {
  switch (initKind) {
  case BodyInitKind::None: out << "none"; return;
  case BodyInitKind::Delegating: out << "delegating"; return;
  case BodyInitKind::Chained: out << "chained"; return;
  case BodyInitKind::ImplicitChained: out << "implicit_chained"; return;
  }
  llvm_unreachable("Bad body init kind");
}

void swift::simple_display(
    llvm::raw_ostream &out, BodyInitKindAndExpr initKindAndExpr) {
  simple_display(out, initKindAndExpr.initKind);
  out << " ";
  simple_display(out, initKindAndExpr.initExpr);
}

//----------------------------------------------------------------------------//
// ResolveMacroRequest computation.
//----------------------------------------------------------------------------//

DeclNameRef UnresolvedMacroReference::getMacroName() const {
  if (auto *expansion = pointer.dyn_cast<FreestandingMacroExpansion *>())
    return expansion->getMacroName();
  if (auto *attr = pointer.dyn_cast<CustomAttr *>()) {
    auto *identTypeRepr = dyn_cast_or_null<IdentTypeRepr>(attr->getTypeRepr());
    if (!identTypeRepr)
      return DeclNameRef();
    return identTypeRepr->getNameRef();
  }
  llvm_unreachable("Unhandled case");
}

SourceLoc UnresolvedMacroReference::getSigilLoc() const {
  if (auto *expansion = pointer.dyn_cast<FreestandingMacroExpansion *>())
    return expansion->getPoundLoc();
  if (auto *attr = pointer.dyn_cast<CustomAttr *>())
    return attr->getRangeWithAt().Start;
  llvm_unreachable("Unhandled case");
}

DeclNameLoc UnresolvedMacroReference::getMacroNameLoc() const {
  if (auto *expansion = pointer.dyn_cast<FreestandingMacroExpansion *>())
    return expansion->getMacroNameLoc();
  if (auto *attr = pointer.dyn_cast<CustomAttr *>()) {
    auto *identTypeRepr = dyn_cast_or_null<IdentTypeRepr>(attr->getTypeRepr());
    if (!identTypeRepr)
      return DeclNameLoc();
    return identTypeRepr->getNameLoc();
  }
  llvm_unreachable("Unhandled case");
}

SourceRange UnresolvedMacroReference::getGenericArgsRange() const {
  if (auto *expansion = pointer.dyn_cast<FreestandingMacroExpansion *>())
    return expansion->getGenericArgsRange();

  if (auto *attr = pointer.dyn_cast<CustomAttr *>()) {
    auto *typeRepr = attr->getTypeRepr();
    auto *genericTypeRepr = dyn_cast_or_null<GenericIdentTypeRepr>(typeRepr);
    if (!genericTypeRepr)
      return SourceRange();

    return genericTypeRepr->getAngleBrackets();
  }

  llvm_unreachable("Unhandled case");
}

ArrayRef<TypeRepr *> UnresolvedMacroReference::getGenericArgs() const {
  if (auto *expansion = pointer.dyn_cast<FreestandingMacroExpansion *>())
    return expansion->getGenericArgs();

  if (auto *attr = pointer.dyn_cast<CustomAttr *>()) {
    auto *typeRepr = attr->getTypeRepr();
    auto *genericTypeRepr = dyn_cast_or_null<GenericIdentTypeRepr>(typeRepr);
    if (!genericTypeRepr)
      return {};

    return genericTypeRepr->getGenericArgs();
  }

  llvm_unreachable("Unhandled case");
}

ArgumentList *UnresolvedMacroReference::getArgs() const {
  if (auto *expansion = pointer.dyn_cast<FreestandingMacroExpansion *>())
    return expansion->getArgs();
  if (auto *attr = pointer.dyn_cast<CustomAttr *>())
    return attr->getArgs();
  llvm_unreachable("Unhandled case");
}

MacroRoles UnresolvedMacroReference::getMacroRoles() const {
  if (pointer.is<FreestandingMacroExpansion *>())
    return getFreestandingMacroRoles();

  if (pointer.is<CustomAttr *>())
    return getAttachedMacroRoles();

  llvm_unreachable("Unsupported macro reference");
}

void swift::simple_display(llvm::raw_ostream &out,
                           const UnresolvedMacroReference &ref) {
  if (ref.getFreestanding())
    out << "freestanding-macro-expansion";
  else if (ref.getAttr())
    out << "custom-attr";
}

void swift::simple_display(llvm::raw_ostream &out, MacroRoles roles) {
  out << "macro-roles";
}

bool swift::operator==(MacroRoles lhs, MacroRoles rhs) {
  return lhs.containsOnly(rhs);
}

llvm::hash_code swift::hash_value(MacroRoles roles) {
  return roles.toRaw();
}

static bool isAttachedSyntax(const UnresolvedMacroReference &ref) {
  return ref.getAttr() != nullptr;
}

void ResolveMacroRequest::diagnoseCycle(DiagnosticEngine &diags) const {
  const auto &storage = getStorage();
  auto macroRef = std::get<0>(storage);
  diags.diagnose(macroRef.getSigilLoc(), diag::macro_resolve_circular_reference,
                 isAttachedSyntax(macroRef),
                 macroRef.getMacroName().getFullName());
}

void ResolveMacroRequest::noteCycleStep(DiagnosticEngine &diags) const {
  const auto &storage = getStorage();
  auto macroRef = std::get<0>(storage);
  diags.diagnose(macroRef.getSigilLoc(),
                 diag::macro_resolve_circular_reference_through,
                 isAttachedSyntax(macroRef),
                 macroRef.getMacroName().getFullName());
}

void ExpandMacroExpansionDeclRequest::diagnoseCycle(DiagnosticEngine &diags) const {
  auto decl = std::get<0>(getStorage());
  diags.diagnose(decl->getPoundLoc(),
                 diag::macro_expand_circular_reference,
                 "freestanding",
                 decl->getMacroName().getFullName());
}

void ExpandMacroExpansionDeclRequest::noteCycleStep(DiagnosticEngine &diags) const {
  auto decl = std::get<0>(getStorage());
  diags.diagnose(decl->getPoundLoc(),
                 diag::macro_expand_circular_reference_through,
                 "freestanding",
                 decl->getMacroName().getFullName());
}

void ExpandMacroExpansionExprRequest::diagnoseCycle(DiagnosticEngine &diags) const {
  auto expr = std::get<0>(getStorage());
  diags.diagnose(expr->getStartLoc(),
                 diag::macro_expand_circular_reference,
                 "freestanding",
                 expr->getMacroName().getFullName());
}

void ExpandMacroExpansionExprRequest::noteCycleStep(DiagnosticEngine &diags) const {
  auto expr = std::get<0>(getStorage());
  diags.diagnose(expr->getStartLoc(),
                 diag::macro_expand_circular_reference_through,
                 "freestanding",
                 expr->getMacroName().getFullName());
}

void ExpandAccessorMacros::diagnoseCycle(DiagnosticEngine &diags) const {
  auto decl = std::get<0>(getStorage());
  diags.diagnose(decl->getLoc(),
                 diag::macro_expand_circular_reference_entity,
                 "accessor",
                 decl->getName());
}

void ExpandAccessorMacros::noteCycleStep(DiagnosticEngine &diags) const {
  auto decl = std::get<0>(getStorage());
  diags.diagnose(decl->getLoc(),
                 diag::macro_expand_circular_reference_entity_through,
                 "accessor",
                 decl->getName());
}

void ExpandExtensionMacros::diagnoseCycle(DiagnosticEngine &diags) const {
  auto decl = std::get<0>(getStorage());
  diags.diagnose(decl->getLoc(),
                 diag::macro_expand_circular_reference_entity,
                 "extension",
                 decl->getName());
}

void ExpandExtensionMacros::noteCycleStep(DiagnosticEngine &diags) const {
  auto decl = std::get<0>(getStorage());
  diags.diagnose(decl->getLoc(),
                 diag::macro_expand_circular_reference_entity_through,
                 "extension",
                 decl->getName());
}

void ExpandMemberAttributeMacros::diagnoseCycle(DiagnosticEngine &diags) const {
  auto decl = std::get<0>(getStorage());
  if (auto value = dyn_cast<ValueDecl>(decl)) {
    diags.diagnose(decl->getLoc(),
                   diag::macro_expand_circular_reference_entity,
                   "member attribute",
                   value->getName());
  } else {
    diags.diagnose(decl->getLoc(),
                   diag::macro_expand_circular_reference_unnamed,
                   "member attribute");
  }
}

void ExpandMemberAttributeMacros::noteCycleStep(DiagnosticEngine &diags) const {
  auto decl = std::get<0>(getStorage());
  if (auto value = dyn_cast<ValueDecl>(decl)) {
    diags.diagnose(decl->getLoc(),
                   diag::macro_expand_circular_reference_entity_through,
                   "member attribute",
                   value->getName());
  } else {
    diags.diagnose(decl->getLoc(),
                   diag::macro_expand_circular_reference_unnamed_through,
                   "member attribute");
  }
}

void ExpandSynthesizedMemberMacroRequest::diagnoseCycle(DiagnosticEngine &diags) const {
  auto decl = std::get<0>(getStorage());
  if (auto value = dyn_cast<ValueDecl>(decl)) {
    diags.diagnose(decl->getLoc(),
                   diag::macro_expand_circular_reference_entity,
                   "member",
                   value->getName());
  } else {
    diags.diagnose(decl->getLoc(),
                   diag::macro_expand_circular_reference_unnamed,
                   "member");
  }
}

void ExpandSynthesizedMemberMacroRequest::noteCycleStep(DiagnosticEngine &diags) const {
  auto decl = std::get<0>(getStorage());
  if (auto value = dyn_cast<ValueDecl>(decl)) {
    diags.diagnose(decl->getLoc(),
                   diag::macro_expand_circular_reference_entity_through,
                   "member",
                   value->getName());
  } else {
    diags.diagnose(decl->getLoc(),
                   diag::macro_expand_circular_reference_unnamed_through,
                   "member");
  }
}

void ExpandPeerMacroRequest::diagnoseCycle(DiagnosticEngine &diags) const {
  auto decl = std::get<0>(getStorage());
  if (auto value = dyn_cast<ValueDecl>(decl)) {
    diags.diagnose(decl->getLoc(),
                   diag::macro_expand_circular_reference_entity,
                   "peer",
                   value->getName());
  } else {
    diags.diagnose(decl->getLoc(),
                   diag::macro_expand_circular_reference_unnamed,
                   "peer");
  }
}

void ExpandPeerMacroRequest::noteCycleStep(DiagnosticEngine &diags) const {
  auto decl = std::get<0>(getStorage());
  if (auto value = dyn_cast<ValueDecl>(decl)) {
    diags.diagnose(decl->getLoc(),
                   diag::macro_expand_circular_reference_entity_through,
                   "peer",
                   value->getName());
  } else {
    diags.diagnose(decl->getLoc(),
                   diag::macro_expand_circular_reference_unnamed_through,
                   "peer");
  }
}
