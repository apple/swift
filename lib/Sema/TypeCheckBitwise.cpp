//===------ TypeCheckBitwise.cpp -  Type checking bitwise protocols -------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2024 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Semantic analysis for BitwiseCopyable.
//
//===----------------------------------------------------------------------===//

#include "TypeCheckBitwise.h"
#include "TypeCheckInvertible.h" // StorageVisitor
#include "TypeChecker.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Builtins.h"
#include "swift/AST/Decl.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/Ownership.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/AST/Types.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/Defer.h"

using namespace swift;

namespace {

enum class BitwiseCopyableCheck {
  /// For a user-written conformance.  Diagnostics must be emitted.
  Explicit,

  /// For a synthesized conformance.  Diagnostics must be suppressed.
  Implicit,
};

bool isImplicit(BitwiseCopyableCheck check) {
  switch (check) {
  case BitwiseCopyableCheck::Explicit:
    return false;

  case BitwiseCopyableCheck::Implicit:
    return true;
  }
}

/// The implicit check kind appropriate to \p nominal, if any.
///
/// For public, non-frozen types, this is ::None.
std::optional<BitwiseCopyableCheck>
getImplicitCheckForNominal(NominalTypeDecl *nominal) {
  assert(nominal);
  if (!nominal
           ->getFormalAccessScope(
               /*useDC=*/nullptr, /*treatUsableFromInlineAsPublic=*/true)
           .isPublicOrPackage())
    return {BitwiseCopyableCheck::Implicit};

  if (nominal->hasClangNode() ||
      nominal->getAttrs().hasAttribute<FixedLayoutAttr>() ||
      nominal->getAttrs().hasAttribute<FrozenAttr>())
    return {BitwiseCopyableCheck::Implicit};

  return std::nullopt;
}

/// Checks that \p nominal conforms to BitwiseCopyable and emits the relevant
/// diagnostics if not.
class BitwiseCopyableStorageVisitor : public StorageVisitor {
public:
  bool invalid = false;

private:
  NominalTypeDecl *const nominal;
  BitwiseCopyableCheck const check;
  DeclContext *const dc;
  ModuleDecl *const module;
  ProtocolDecl *const protocol;

public:
  BitwiseCopyableStorageVisitor(NominalTypeDecl *nominal, DeclContext *dc,
                                BitwiseCopyableCheck check)
      : StorageVisitor(), nominal(nominal), check(check), dc(dc),
        module(dc->getParentModule()),
        protocol(module->getASTContext().getProtocol(
            KnownProtocolKind::BitwiseCopyable)) {
    assert(protocol);
  }

private:
  ValueDecl *storage = nullptr;

  /// Handle a stored property.
  bool operator()(VarDecl *property, Type propertyType) override {
    return visitMemberDecl(property, propertyType);
  }

  /// Handle an enum associated value.
  bool operator()(EnumElementDecl *element, Type elementType) override {
    return visitMemberDecl(element, elementType);
  }

  bool visitMemberDecl(ValueDecl *storage, Type ty);
  bool visitMemberType(Type type, SourceLoc loc);
  bool visitNonconformingMemberType(Type type, SourceLoc loc);
  void emitNonconformingMemberTypeDiagnostic(Type ty, SourceLoc loc);
};

bool BitwiseCopyableStorageVisitor::visitMemberDecl(ValueDecl *decl, Type ty) {
  storage = decl;
  SWIFT_DEFER { storage = nullptr; };

  auto *element = dyn_cast<EnumElementDecl>(decl);
  if (element && element->isIndirect()) {
    if (!isImplicit(check)) {
      nominal->diagnose(diag::non_bitwise_copyable_type_indirect_enum_element);
      element->diagnose(diag::note_non_bitwise_copyable_type_indirect_enum_element);
    }
    invalid = true;
    return true;
  }

  // Fields with unowned(unsafe) ownership are bitwise-copyable.
  auto *roa = decl->getAttrs().getAttribute<ReferenceOwnershipAttr>();
  if (roa && roa->get() == ReferenceOwnership::Unmanaged) {
    return false;
  }

  visitMemberType(ty, storage->getLoc());

  if (invalid) {
    assert(isImplicit(check));
    return true;
  }

  return false;
}

bool BitwiseCopyableStorageVisitor::visitMemberType(Type ty, SourceLoc loc) {
  auto conformance = module->checkConformance(ty, protocol);
  if (conformance.isInvalid() || conformance.hasUnavailableConformance()) {
    return visitNonconformingMemberType(ty, loc);
  }

  // Walk the conformance, diagnosing any missing BitwiseCopyable conformances.
  bool anyMissing = false;
  conformance.forEachMissingConformance(
      [&](BuiltinProtocolConformance *missing) {
        if (visitNonconformingMemberType(missing->getType(), loc)) {
          anyMissing = true;
        }

        return false;
      });

  return anyMissing;
}

bool BitwiseCopyableStorageVisitor::visitNonconformingMemberType(
    Type ty, SourceLoc loc) {
  if (ty->hasError())
    return false;

  if (isImplicit(check)) {
    invalid = true;
    return true;
  }

  emitNonconformingMemberTypeDiagnostic(ty, loc);

  return false;
}

void addBitwiseCopyableFixIt(const TypeDecl *genericArgument,
                             InFlightDiagnostic &diag) {
  if (genericArgument->getInherited().empty()) {
    auto fixItLoc = genericArgument->getLoc();
    diag.fixItInsertAfter(fixItLoc, ": BitwiseCopyable");
  } else {
    auto fixItLoc = genericArgument->getInherited().getEndLoc();
    diag.fixItInsertAfter(fixItLoc, ", BitwiseCopyable");
  }
}

void BitwiseCopyableStorageVisitor::emitNonconformingMemberTypeDiagnostic(
    Type ty, SourceLoc loc) {
  auto module = dc->getParentModule();
  auto memberTypeDecl = ty->getAnyNominal();
  auto &ctx = module->getASTContext();

  storage->diagnose(diag::non_bitwise_copyable_type_member, ty,
                    isa<EnumElementDecl>(storage), storage->getName(), nominal);

  if (ty->is<FunctionType>()) {
    ctx.Diags.diagnose(loc, diag::non_bitwise_copyable_function_type);
  } else if (memberTypeDecl && memberTypeDecl->getParentModule() == module) {
    // If the nominal type is in the current module, suggest adding
    // `BitwiseCopyable` if it might make sense. Otherwise, just complain.
    if (isa<StructDecl>(memberTypeDecl) || isa<EnumDecl>(memberTypeDecl)) {
      auto note = memberTypeDecl->diagnose(
          diag::add_nominal_bitwise_copyable_conformance, memberTypeDecl);
      addBitwiseCopyableFixIt(memberTypeDecl, note);
    } else {
      memberTypeDecl->diagnose(diag::non_bitwise_copyable_nominal,
                               memberTypeDecl);
    }
  } else if (memberTypeDecl) {
    // Note which nominal type does not conform to `BitwiseCopyable`.
    memberTypeDecl->diagnose(diag::non_bitwise_copyable_nominal,
                             memberTypeDecl);
  } else if (auto genericArchetype = ty->getAs<ArchetypeType>()) {
    auto interfaceType = genericArchetype->getInterfaceType();
    if (auto genericParamType = interfaceType->getAs<GenericTypeParamType>()) {
      auto *genericParamTypeDecl = genericParamType->getDecl();
      if (genericParamTypeDecl &&
          genericParamTypeDecl->getModuleContext() == module) {
        auto diag = genericParamTypeDecl->diagnose(
            diag::add_generic_parameter_non_bitwise_copyable_conformance, ty);
        addBitwiseCopyableFixIt(genericParamTypeDecl, diag);
      }
    }
  }
}

static bool checkBitwiseCopyableInstanceStorage(NominalTypeDecl *nominal,
                                                DeclContext *dc,
                                                BitwiseCopyableCheck check) {
  auto *conformanceDecl = dc->getAsDecl() ? dc->getAsDecl() : nominal;
  if (nominal->suppressesConformance(KnownProtocolKind::BitwiseCopyable)) {
    if (!isImplicit(check)) {
      conformanceDecl->diagnose(diag::non_bitwise_copyable_type_suppressed);
    }
    return true;
  }

  auto &attrs = nominal->getAttrs();
  if (attrs.hasAttribute<SensitiveAttr>()) {
    if (!isImplicit(check)) {
      conformanceDecl->diagnose(diag::non_bitwise_copyable_type_sensitive);
    }
    return true;
  }

  if (dc->mapTypeIntoContext(nominal->getDeclaredInterfaceType())
          ->isNoncopyable()) {
    // Already separately diagnosed when explicit.
    return true;
  }

  assert(dc->getParentModule()->getASTContext().getProtocol(
      KnownProtocolKind::BitwiseCopyable));

  if (isa<ClassDecl>(nominal)) {
    if (!isImplicit(check)) {
      nominal->diagnose(diag::non_bitwise_copyable_type_class);
    }
    return true;
  }

  auto *ed = dyn_cast<EnumDecl>(nominal);
  if (ed && ed->isIndirect()) {
    if (!isImplicit(check)) {
      nominal->diagnose(diag::non_bitwise_copyable_type_indirect_enum);
    }
    return true;
  }

  auto *sd = dyn_cast<StructDecl>(nominal);
  if (sd && sd->isCxxNonTrivial()) {
    if (!isImplicit(check)) {
      nominal->diagnose(diag::non_bitwise_copyable_type_cxx_nontrivial);
    }
    return true;
  }

  if (sd && sd->hasUnreferenceableStorage()) {
    if (!isImplicit(check)) {
      sd->diagnose(diag::non_bitwise_copyable_c_type_nontrivial);
      sd->diagnose(diag::note_non_bitwise_copyable_c_type_add_attr);
    }
    return true;
  }

  BitwiseCopyableStorageVisitor visitor(nominal, dc, check);

  return visitor.visit(nominal, dc) || visitor.invalid;
}

struct DeriveImplicitBitwiseCopyableConformance {
  ASTContext &context;
  ProtocolDecl *const protocol;
  NominalTypeDecl *nominal;
  BitwiseCopyableCheck const check;

  DeriveImplicitBitwiseCopyableConformance(NominalTypeDecl *nominal,
                                           BitwiseCopyableCheck check)
      : context(nominal->getASTContext()),
        protocol(context.getProtocol(KnownProtocolKind::BitwiseCopyable)),
        nominal(nominal), check(check) {}

  ProtocolConformance *derive();

private:
  NormalProtocolConformance *synthesizeConformance(DeclContext *dc);
  bool allowedForFile();
};

ProtocolConformance *DeriveImplicitBitwiseCopyableConformance::derive() {
  assert(isImplicit(check));

  if (!allowedForFile())
    return nullptr;

  // If `nominal` could be conformed explicitly to BitwiseCopyable, synthesize
  // an implicit conformance.
  if (checkBitwiseCopyableInstanceStorage(nominal, nominal, check))
    return nullptr;

  return synthesizeConformance(nominal);
}

/// Whether the FileUnitKind and SourceFileKind where `nominal` appears permit
/// a conformance to be derived.
bool DeriveImplicitBitwiseCopyableConformance::allowedForFile() {
  auto *file = dyn_cast<FileUnit>(nominal->getModuleScopeContext());
  if (!file)
    return false;

  switch (file->getKind()) {
  case FileUnitKind::Source: {
    auto sourceFile = nominal->getParentSourceFile();
    if (!sourceFile)
      return true;

    switch (sourceFile->Kind) {
    case SourceFileKind::Interface:
      // Other native modules' types' conformances were inferred (or defined
      // explicitly) during their modules' compilation.
      return false;

    case SourceFileKind::Library:
    case SourceFileKind::MacroExpansion:
    case SourceFileKind::DefaultArgument:
    case SourceFileKind::Main:
    case SourceFileKind::SIL:
      return true;
    }
    llvm_unreachable("covered switch");
  }
  case FileUnitKind::Builtin:
    // Conformances were explicitly added to builtin types in
    // getBuiltinBuiltinTypeConformance.
    return false;
  case FileUnitKind::SerializedAST:
    // Conformance is serialized along with the type; if it's absent, do not
    // infer it retroactively.
    return false;

  case FileUnitKind::Synthesized:
    // Infer conformance for types in the synthesized file.
    return true;

  case FileUnitKind::ClangModule:
  case FileUnitKind::DWARFModule:
    // Infer conformance for imported foreign modules.
    return true;
  }
}

NormalProtocolConformance *
DeriveImplicitBitwiseCopyableConformance::synthesizeConformance(
    DeclContext *dc) {
  auto conformance = context.getNormalConformance(
      nominal->getDeclaredInterfaceType(), protocol, nominal->getLoc(), dc,
      ProtocolConformanceState::Complete,
      /*isUnchecked=*/false,
      /*isPreconcurrency=*/false);
  conformance->setSourceKindAndImplyingConformance(
      ConformanceEntryKind::Synthesized, nullptr);

  nominal->registerProtocolConformance(conformance, /*synthesized=*/true);
  return conformance;
}
} // end anonymous namespace

ProtocolConformance *
swift::deriveImplicitBitwiseCopyableConformance(NominalTypeDecl *nominal) {
  assert(
      nominal->getASTContext().getProtocol(KnownProtocolKind::BitwiseCopyable));
  auto check = getImplicitCheckForNominal(nominal);
  if (!check)
    return nullptr;
  return DeriveImplicitBitwiseCopyableConformance(nominal, *check).derive();
}

bool swift::checkBitwiseCopyableConformance(ProtocolConformance *conformance,
                                            bool isImplicit) {
  assert(conformance->getProtocol() ==
         conformance->getDeclContext()
             ->getParentModule()
             ->getASTContext()
             .getProtocol(KnownProtocolKind::BitwiseCopyable));
  auto conformanceDC = conformance->getDeclContext();
  auto nominal = conformance->getType()->getAnyNominal();
  if (!nominal)
    return false;

  // If this is an always-unavailable conformance, there's nothing to check.
  if (auto ext = dyn_cast<ExtensionDecl>(conformanceDC)) {
    if (AvailableAttr::isUnavailable(ext))
      return false;
  }

  // BitwiseCopyable must be added in the same source file.
  auto conformanceDecl = conformanceDC->getAsDecl();
  if (conformanceDecl->getModuleContext() != nominal->getModuleContext()) {
    conformanceDecl->diagnose(diag::bitwise_copyable_outside_module, nominal);
    return true;
  }

  auto check = isImplicit ? BitwiseCopyableCheck::Implicit
                          : BitwiseCopyableCheck::Explicit;

  return checkBitwiseCopyableInstanceStorage(nominal, conformanceDC, check);
}
