//===--- TypeCheckInvertible.cpp -  Type checking invertible protocols ----===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements semantic analysis for evaluating whether a type
// conforms to an invertible protocol. An invertible protocol is a known
// protocol KP for which the type ~KP exists.
//
//===----------------------------------------------------------------------===//

#include "TypeCheckInvertible.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/InverseMarking.h"
#include "TypeChecker.h"

namespace swift {

/// MARK: diagnostic utilities

/// Adds the appropriate fix-it to make the given nominal conform to \c proto.
static void addConformanceFixIt(const NominalTypeDecl *nominal,
                                InFlightDiagnostic &diag,
                                KnownProtocolKind proto,
                                bool inverse) {
  SmallString<64> text;
  if (nominal->getInherited().empty()) {
    SourceLoc fixItLoc = nominal->getBraces().Start;
    text.append(": ");
    if (inverse) text.append("~");
    text.append(getProtocolName(proto));
    diag.fixItInsert(fixItLoc, text);
  } else {
    auto fixItLoc = nominal->getInherited().getEndLoc();
    text.append(", ");
    if (inverse) text.append("~");
    text.append(getProtocolName(proto));
    diag.fixItInsertAfter(fixItLoc, text);
  }
}

// If there is not already an inverse ~KP applied to this type, suggest it.
// The goal here is that we want to tell users how they can suppress or remove
// a conformance to KP.
static void emitAdviceToApplyInverseAfter(InFlightDiagnostic &&diag,
                                          InvertibleProtocolKind ip,
                                          InverseMarking marking,
                                          NominalTypeDecl *nominal) {
  auto kp = getKnownProtocolKind(ip);

  // Immediately flush, then emit notes, so they're associated.
  diag.flush();

  auto &ctx = nominal->getASTContext();
  // Not expecting the positive KP constraint to be classified as "Inferred".
  assert(marking.getPositive().getKind() != InverseMarking::Kind::Inferred);

  // Have no advice for situations where the KP conformance is explicit.
  if (marking.getPositive().isPresent())
    return;

  switch (marking.getInverse().getKind()) {
  case InverseMarking::Kind::Inferred:
    // Note that the enclosing type is conditionally conforming to KP first.
    ctx.Diags.diagnose(marking.getInverse().getLoc(),
                       diag::note_inverse_preventing_conformance_implicit,
                       nominal, getProtocolName(kp));
    LLVM_FALLTHROUGH;
  case InverseMarking::Kind::None: {
    // Suggest adding ~KP to make it non-KP.
    auto diag = nominal->diagnose(diag::add_inverse,
                                       nominal,
                                       getProtocolName(kp));
    addConformanceFixIt(nominal, diag, kp, /*inverse=*/true);
  }
    break;
  case InverseMarking::Kind::LegacyExplicit:
  case InverseMarking::Kind::Explicit:
    // FIXME: we can probably do better here. Look for the extension where the
    // inverse came from.
    break;
  }
}

/// Emit fix-it's to help the user resolve a containment issue where the
/// \c nonConformingTy needs to be made to conform to \c kp to resolve a
/// containment issue.
/// \param enclosingNom is the nominal type containing a nonconforming value
/// \param nonConformingTy is the type of the nonconforming value
static void tryEmitContainmentFixits(InFlightDiagnostic &&diag,
                                     NominalTypeDecl *enclosingNom,
                                     Type nonConformingTy,
                                     InvertibleProtocolKind ip) {
  auto *module = enclosingNom->getParentModule();
  auto &ctx = enclosingNom->getASTContext();
  auto kp = getKnownProtocolKind(ip);

  // Check the enclosing type's markings to see what to suggest.
  auto enclosingMarking = enclosingNom->getMarking(ip);

  // First, the generic advice.
  emitAdviceToApplyInverseAfter(std::move(diag), ip,
                                enclosingMarking, enclosingNom);

  // If it's a generic parameter defined in the same module, point to the
  // parameter that must have had the inverse applied to it somewhere.
  if (auto genericArchetype = nonConformingTy->getAs<ArchetypeType>()) {
    auto interfaceType = genericArchetype->getInterfaceType();
    if (auto genericParamType =
        interfaceType->getAs<GenericTypeParamType>()) {
      auto *genericParamTypeDecl = genericParamType->getDecl();
      if (genericParamTypeDecl &&
          genericParamTypeDecl->getModuleContext() == module) {
        genericParamTypeDecl->diagnose(
            diag::note_inverse_preventing_conformance,
            nonConformingTy, getProtocolName(kp));
      }
    }
    return;
  }

  // If the offending type is a nominal with a SourceLoc, explain why it's
  // not IP.
  if (auto nominal = nonConformingTy->getAnyNominal()) {
    if (nominal->getLoc(/*SerializedOK=*/false)) {
      auto inverse = nominal->getMarking(ip).getInverse();
      auto loc = inverse.getLoc();

      switch (inverse.getKind()) {
      case InverseMarking::Kind::None:
        assert(false && "how did it become noncopyable/nonescapable then?");
        break;
      case InverseMarking::Kind::Inferred:
        assert(loc);
        ctx.Diags.diagnose(loc,
                           diag::note_inverse_preventing_conformance_implicit,
                           nominal, getProtocolName(kp));
        break;
      case InverseMarking::Kind::LegacyExplicit:
      case InverseMarking::Kind::Explicit:
        assert(loc);
        ctx.Diags.diagnose(loc,
                           diag::note_inverse_preventing_conformance_explicit,
                           nominal, getProtocolName(kp));
        break;
      }
    }
  }
}

/// MARK: conformance queries

static bool conformsToInvertible(CanType type, InvertibleProtocolKind ip) {
  auto &ctx = type->getASTContext();

  auto *proto = ctx.getProtocol(getKnownProtocolKind(ip));
  assert(proto && "missing Copyable/Escapable from stdlib!");

  // Must not have a type parameter!
  assert(!type->hasTypeParameter() && "caller forgot to mapTypeIntoContext!");

  assert(!type->is<PackExpansionType>());

  // The SIL types in the AST do not have real conformances, and should have
  // been handled in SILType instead.
  assert(!(type->is<SILBoxType,
                    SILMoveOnlyWrappedType,
                    SILPackType,
                    SILTokenType>()));

  const bool conforms =
      (bool) proto->getParentModule()->checkConformance(
          type, proto,
          /*allowMissing=*/false);

  return conforms;
}

bool IsEscapableRequest::evaluate(Evaluator &evaluator,
                                  CanType type) const {
  return conformsToInvertible(type, InvertibleProtocolKind::Escapable);
}

bool IsNoncopyableRequest::evaluate(Evaluator &evaluator,
                                    CanType type) const {
  return !conformsToInvertible(type, InvertibleProtocolKind::Copyable);
}

/// MARK: conformance checking
static bool checkInvertibleConformanceCommon(ProtocolConformance *conformance,
                                             InvertibleProtocolKind ip) {
  const auto kp = getKnownProtocolKind(ip);
  auto *proto = conformance->getProtocol();
  assert(proto->isSpecificProtocol(kp));

  auto *nom = conformance->getType()->getAnyNominal();
  assert(nom && "non-nominal with conformance?");
  if (!nom)
    return false;

  auto &ctx = nom->getASTContext();
  bool conforms = true;

  // An explicit `~IP` prevents conformance if any of these are true:
  //
  // 1. It appears on a class.
  // 2. Appears on the same declaration that also declares the conformance.
  //    So, if the nominal has `~Copyable` but this conformance is
  //    written in an extension, then we do not raise an error.
  auto marking = nom->getMarking(ip);
  if (marking.getInverse().isPresent()) {
    if (isa<ClassDecl>(nom)) {
      ctx.Diags.diagnose(marking.getInverse().getLoc(),
                         diag::inverse_on_class,
                         getProtocolName(kp));
      conforms &= false;
    } else if (conformance->getDeclContext() == nom) {
      ctx.Diags.diagnose(marking.getInverse().getLoc(),
                         diag::inverse_but_also_conforms,
                         nom, getProtocolName(kp));
      conforms &= false;
    }
  }

  // All classes can store noncopyable/nonescaping values.
  if (isa<ClassDecl>(nom))
    return conforms;

  // Protocols do not directly define any storage.
  if (isa<ProtocolDecl, BuiltinTupleDecl>(nom))
    llvm_unreachable("unexpected nominal to check Copyable conformance");

  // A deinit prevents a struct or enum from conforming to Copyable.
  if (ip == InvertibleProtocolKind::Copyable) {
    if (auto *deinit = nom->getValueTypeDestructor()) {
      auto diag = deinit->diagnose(diag::copyable_illegal_deinit, nom);
      emitAdviceToApplyInverseAfter(std::move(diag),
                                    ip,
                                    nom->getMarking(ip),
                                    nom);
      conforms &= false;
    }
  }

  // Otherwise, we have to check its storage to ensure it is all
  // Copyable/Escapable.

  class LacksMatchingStorage: public StorageVisitor {
    NominalTypeDecl *Nominal;
    DeclContext *DC;
    InvertibleProtocolKind IP;
    bool Diagnosing;
  public:
    LacksMatchingStorage(NominalTypeDecl *nom,
                         DeclContext *dc,
                         InvertibleProtocolKind ip,
                         bool diagnose)
        : Nominal(nom), DC(dc), IP(ip), Diagnosing(diagnose) {}

    bool visit() { return StorageVisitor::visit(Nominal, DC); }

    bool check(ValueDecl *storage, Type type, bool isEnum) {
      // ignore invalid storage.
      if (type->hasError())
        return false;

      // For a type conforming to IP, ensure that the storage conforms to IP.
      switch (IP) {
      case InvertibleProtocolKind::Copyable:
        if (!type->isNoncopyable())
          return false;
        break;
      case InvertibleProtocolKind::Escapable:
        if (type->isEscapable())
          return false;
        break;
      }

      if (!Diagnosing)
        return true; // it's got storage missing conformance to IP

      auto diag =
          storage->diagnose(diag::inverse_type_member_in_conforming_type,
                            type, isEnum, storage->getName(), Nominal,
                            getProtocolName(getKnownProtocolKind(IP)));

      tryEmitContainmentFixits(std::move(diag), Nominal, type, IP);
      return true;
    }

    /// Handle a stored property.
    /// \returns true iff this visitor should stop its walk over the nominal.
    bool operator()(VarDecl *property, Type propertyType) override {
      return check(property, propertyType, /*isEnum=*/false);
    }

    /// Handle an enum associated value.
    /// \returns true iff this visitor should stop its walk over the nominal.
    bool operator()(EnumElementDecl *element, Type elementType) override {
      return check(element, elementType, /*isEnum=*/true);
    }
  };

  // This nominal cannot conform to IP if it contains storage that does not
  // conform to IP.
  bool lacksMatchingStorage =
      LacksMatchingStorage(nom, conformance->getDeclContext(),
                           ip, /*diagnose=*/true).visit();
  conforms &= !lacksMatchingStorage;

  return conforms;
}

bool checkEscapableConformance(ProtocolConformance *conformance) {
  return checkInvertibleConformanceCommon(conformance,
                                          InvertibleProtocolKind::Escapable);
}

bool checkCopyableConformance(ProtocolConformance *conformance) {
  return checkInvertibleConformanceCommon(conformance,
                                          InvertibleProtocolKind::Copyable);
}

/// Visit the instance storage of the given nominal type as seen through
/// the given declaration context.
bool StorageVisitor::visit(NominalTypeDecl *nominal, DeclContext *dc) {
  // Walk the stored properties of classes and structs.
  if (isa<StructDecl>(nominal) || isa<ClassDecl>(nominal)) {
    for (auto property : nominal->getStoredProperties()) {
      auto propertyType = dc->mapTypeIntoContext(
          property->getValueInterfaceType());
      if ((*this)(property, propertyType))
        return true;
    }

    return false;
  }

  // Walk the enum elements that have associated values.
  if (auto enumDecl = dyn_cast<EnumDecl>(nominal)) {
    for (auto caseDecl : enumDecl->getAllCases()) {
      for (auto element : caseDecl->getElements()) {
        if (!element->hasAssociatedValues())
          continue;

        // Check that the associated value type is Sendable.
        auto elementType = dc->mapTypeIntoContext(
            element->getArgumentInterfaceType());
        if ((*this)(element, elementType))
          return true;
      }
    }

    return false;
  }

  assert(!isa<ProtocolDecl>(nominal) || !isa<BuiltinTupleDecl>(nominal));
  return false;
}

/// Produces implicit ProtocolConformances for known protocols. Does _not_ check
/// whether the conformance is valid. Nor does it recursively check whether
/// stored properties implicitly conform, so there is no risk of a
/// request-evaluator cycle.
///
/// (the conformance is checked in `TypeChecker::checkConformancesInContext`).
ProtocolConformance *deriveConformanceForInvertible(Evaluator &evaluator,
                                                    NominalTypeDecl *nominal,
                                                    KnownProtocolKind kp) {
  auto &ctx = nominal->getASTContext();
  auto *proto = ctx.getProtocol(kp);
  auto ip = getInvertibleProtocolKind(kp);
  if (!ip)
    llvm_unreachable("not an invertible protocol");

  auto file = cast<FileUnit>(nominal->getModuleScopeContext());

  // Generates a conformance for the nominal to the protocol.
  // The conformanceDC specifies THE decl context to use for the conformance.
  auto generateConformance =
      [&](DeclContext *conformanceDC) -> ProtocolConformance * {
    // Form a conformance.
    auto conformance = ctx.getNormalConformance(
        nominal->getDeclaredInterfaceType(), proto, nominal->getLoc(),
        conformanceDC, ProtocolConformanceState::Complete,
        /*isUnchecked=*/false, /*isPreconcurrency=*/false);
    conformance->setSourceKindAndImplyingConformance(
        ConformanceEntryKind::Synthesized, nullptr);

    nominal->registerProtocolConformance(conformance, /*synthesized=*/true);
    return conformance;
  };

  auto generateConditionalConformance = [&]() -> ProtocolConformance * {
    // Generate an extension with a conditional conformance to IP that
    // requires all generic parameters to be IP.
    auto protoTy = proto->getDeclaredInterfaceType();
    auto dc = nominal->getDeclContext();

    // extension Nominal: P { ... }
    SmallVector<InheritedEntry, 1> inherited;
    inherited.emplace_back(TypeLoc::withoutLoc(protoTy));
    auto *ext = ExtensionDecl::create(ctx, SourceLoc(), nullptr,
                                      ctx.AllocateCopy(inherited),
                                      dc, nullptr);
    ext->setImplicit();

    // Build a generic signature for this extension that looks like this:
    // <T_1..., T_n where T_1: IP, ... T_n: IP>
    auto genericSig = nominal->getGenericSignature();
    auto params = genericSig.getGenericParams();
    SmallVector<Requirement, 2> reqs;

    for (auto param : params)
      reqs.push_back({RequirementKind::Conformance, param, protoTy});

    genericSig = buildGenericSignature(ctx, genericSig, {}, reqs);
    ext->setGenericSignature(genericSig);

    // Bind the extension.
    evaluator.cacheOutput(ExtendedTypeRequest{ext},
                          nominal->getDeclaredInterfaceType());
    ext->setExtendedNominal(nominal);
    nominal->addExtension(ext);

    // Make it accessible to getTopLevelDecls() so it gets type-checked.
    file->getOrCreateSynthesizedFile().addTopLevelDecl(ext);

    // Then create the conformance using the extension as the conformance's
    // DeclContext, which is how we register these conditional requirements
    // with the conformance.
    return generateConformance(ext);
  };

  switch (*ip) {
  case InvertibleProtocolKind::Copyable:
    // If move-only classes is enabled, we'll check the markings.
    if (ctx.LangOpts.hasFeature(Feature::MoveOnlyClasses))
      break;

  LLVM_FALLTHROUGH;
  case InvertibleProtocolKind::Escapable:
    // Always derive unconditional IP conformance for classes
    if (isa<ClassDecl>(nominal))
      return generateConformance(nominal);
    break;
  }

  auto marking = nominal->getMarking(*ip);

  // Unexpected to have any positive marking for IP if we're deriving it.
  assert(!marking.getPositive().isPresent());

  // Check what kind of inverse-marking we have to determine whether to generate
  // a conformance for IP.
  switch (marking.getInverse().getKind()) {
  case InverseMarking::Kind::LegacyExplicit:
  case InverseMarking::Kind::Explicit:
    return nullptr; // No positive IP conformance will be inferred.

  case InverseMarking::Kind::Inferred:
    if (!isa<ClassDecl>(nominal))
      return generateConditionalConformance();

  LLVM_FALLTHROUGH;
  case InverseMarking::Kind::None:
    // All types already start with conformances to the invertible protocols in
    // this case, within `NominalTypeDecl::prepareConformanceTable`.
    //
    // There are various other kinds of SourceFiles, like SIL, which instead
    // get their conformances here instead.
    //
    // If there's no inverse, we infer a positive IP conformance.
    return generateConformance(nominal);
  }
}

}
