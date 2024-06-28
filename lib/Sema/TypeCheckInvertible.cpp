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
#include "swift/Basic/Assertions.h"
#include "TypeChecker.h"

using namespace swift;

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
static void emitAdviceToApplyInverseAfter(InvertibleProtocolKind ip,
                                          bool canAddInverse,
                                          NominalTypeDecl *nominal) {
  auto kp = getKnownProtocolKind(ip);

  if (canAddInverse) {
    auto diag = nominal->diagnose(diag::add_inverse,
                                  nominal,
                                  getProtocolName(kp));
    addConformanceFixIt(nominal, diag, kp, /*inverse=*/true);
  }
}

/// Emit fix-it's to help the user resolve a containment issue where the
/// \c nonConformingTy needs to be made to conform to \c kp to resolve a
/// containment issue.
/// \param enclosingNom is the nominal type containing a nonconforming value
/// \param nonConformingTy is the type of the nonconforming value
static void tryEmitContainmentFixits(NominalTypeDecl *enclosingNom,
                                     bool canAddInverse,
                                     Type nonConformingTy,
                                     InvertibleProtocolKind ip) {
  auto *module = enclosingNom->getParentModule();
  auto &ctx = enclosingNom->getASTContext();
  auto kp = getKnownProtocolKind(ip);

  // First, the generic advice.
  emitAdviceToApplyInverseAfter(ip, canAddInverse, enclosingNom);

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
      ctx.Diags.diagnose(nominal->getLoc(),
                         diag::note_inverse_preventing_conformance_explicit,
                         nominal, getProtocolName(kp));
    }
  }
}

/// MARK: conformance checking
static void checkInvertibleConformanceCommon(DeclContext *dc,
                                             ProtocolConformanceRef conformance,
                                             InvertibleProtocolKind ip) {
  assert(!conformance.isInvalid());

  const auto kp = getKnownProtocolKind(ip);
  assert(conformance.getRequirement()->isSpecificProtocol(kp));

  auto *nominalDecl = dc->getSelfNominalTypeDecl();
  assert(isa<StructDecl>(nominalDecl) ||
         isa<EnumDecl>(nominalDecl) ||
         isa<ClassDecl>(nominalDecl));

  auto &ctx = nominalDecl->getASTContext();

  InvertibleProtocolSet inverses;
  bool anyObject = false;
  (void) getDirectlyInheritedNominalTypeDecls(nominalDecl, inverses, anyObject);

  // Handle deprecated attributes.
  if (nominalDecl->getAttrs().hasAttribute<MoveOnlyAttr>())
    inverses.insert(InvertibleProtocolKind::Copyable);
  if (nominalDecl->getAttrs().hasAttribute<NonEscapableAttr>())
    inverses.insert(InvertibleProtocolKind::Escapable);

  bool hasExplicitInverse = inverses.contains(ip);

  bool hasUnconditionalConformance = conformance.isAbstract();
  SourceLoc conformanceLoc = nominalDecl->getLoc();

  if (conformance.isConcrete()) {
    auto concrete = conformance.getConcrete();
    if (auto *normalConf = dyn_cast<NormalProtocolConformance>(concrete)) {
      conformanceLoc = normalConf->getLoc();
      assert(conformanceLoc);

      auto condReqs = normalConf->getConditionalRequirements();
      hasUnconditionalConformance = condReqs.empty();
      auto *thisProto = normalConf->getProtocol();

      // Ensure that conditional conformance to an invertible protocol IP only
      // depends conformance requirements involving IP, and its subject is not
      // a dependent member type.
      //
      // In theory, it could depend on any invertible protocol, but it may be
      // confusing if we permitted that and this simplifies the model a bit.
      for (auto req : condReqs) {
        Type illegalSecondType;

        // If we are diagnosing, fill-in the second-type string of this req.
        switch (req.getKind()) {
        case RequirementKind::Layout:
          assert(req.getLayoutConstraint()->isClass());
          illegalSecondType = ctx.getAnyObjectType();
          break;
        case RequirementKind::Conformance:
          if (req.getProtocolDecl() == thisProto
              && !req.getFirstType()->is<DependentMemberType>())
            break; // permitted, don't fill-in.
        LLVM_FALLTHROUGH;
        case RequirementKind::Superclass:
        case RequirementKind::SameType:
        case RequirementKind::SameShape:
          illegalSecondType = req.getSecondType();
          break;
        }

        static_assert((unsigned)RequirementKind::LAST_KIND == 4,
                      "update %select in diagnostic!");
        if (illegalSecondType) {
          auto t = ctx.Diags.diagnose(conformanceLoc,
                             diag::inverse_cannot_be_conditional_on_requirement,
                             thisProto,
                             req.getFirstType(),
                             static_cast<unsigned>(req.getKind()),
                             illegalSecondType);
        }
      }
    }
  }
  assert(!conformance.isPack() && "not handled");

  if (!isa<ClassDecl>(nominalDecl) ||
      ctx.LangOpts.hasFeature(Feature::MoveOnlyClasses)) {
    // If the inheritance clause contains ~Copyable, reject an unconditional
    // conformance to Copyable.
    if (hasExplicitInverse && hasUnconditionalConformance) {
      ctx.Diags.diagnose(conformanceLoc,
                         diag::inverse_but_also_conforms,
                         nominalDecl, getProtocolName(kp));
    }
  }

  // All classes can store noncopyable/nonescaping values.
  if (isa<ClassDecl>(nominalDecl))
    return;

  bool canAddInverse = !hasExplicitInverse && !hasUnconditionalConformance;

  // A deinit prevents a struct or enum from conforming to Copyable.
  if (ip == InvertibleProtocolKind::Copyable) {
    if (auto *deinit = nominalDecl->getValueTypeDestructor()) {
      deinit->diagnose(diag::copyable_illegal_deinit, nominalDecl);
      emitAdviceToApplyInverseAfter(ip, canAddInverse, nominalDecl);
    }
  }

  // Check storage for conformance to Copyable/Escapable.

  class LacksMatchingStorage: public StorageVisitor {
    NominalTypeDecl *Nominal;
    DeclContext *DC;
    InvertibleProtocolKind IP;
    bool CanAddInverse;
  public:
    LacksMatchingStorage(NominalTypeDecl *nom,
                         DeclContext *dc,
                         bool canAddInverse,
                         InvertibleProtocolKind ip)
        : Nominal(nom), DC(dc), IP(ip),
          CanAddInverse(canAddInverse) {}

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

      storage->diagnose(diag::inverse_type_member_in_conforming_type,
                        type, isEnum, storage->getName(), Nominal,
                        getProtocolName(getKnownProtocolKind(IP)));

      tryEmitContainmentFixits(Nominal, CanAddInverse, type, IP);
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
  LacksMatchingStorage(nominalDecl, dc, canAddInverse, ip).visit();
}

void swift::checkEscapableConformance(DeclContext *dc,
                                      ProtocolConformanceRef conformance) {
  checkInvertibleConformanceCommon(dc, conformance,
                                   InvertibleProtocolKind::Escapable);
}

void swift::checkCopyableConformance(DeclContext *dc,
                                     ProtocolConformanceRef conformance) {
  checkInvertibleConformanceCommon(dc, conformance,
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
