//===--- ResilienceDiagnostics.cpp - Resilience Inlineability Diagnostics -===//
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
// This file implements diagnostics for fragile functions, like those with
// @inlinable, @_alwaysEmitIntoClient, or @backDeployed.
//
//===----------------------------------------------------------------------===//

#include "TypeChecker.h"
#include "TypeCheckAvailability.h"
#include "TypeCheckAccess.h"
#include "swift/AST/Attr.h"
#include "swift/AST/Decl.h"
#include "swift/AST/DeclContext.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/TypeDeclFinder.h"
#include "swift/Basic/Assertions.h"

using namespace swift;

static bool addMissingImport(SourceLoc loc, const Decl *D,
                             const ExportContext &where) {
  ASTContext &ctx = where.getDeclContext()->getASTContext();
  ModuleDecl *M = D->getModuleContext();
  auto *SF = where.getDeclContext()->getParentSourceFile();

  // Only add imports of API level modules if this is an API level module.
  if (M->getLibraryLevel() != LibraryLevel::API &&
      SF->getParentModule()->getLibraryLevel() == LibraryLevel::API)
    return false;

  // Hack to fix swiftinterfaces in case of missing imports. We can get rid of
  // this logic when we don't leak the use of non-locally imported things in
  // API.
  auto missingImport = ImportedModule(ImportPath::Access(),
                                      const_cast<ModuleDecl *>(M));
  SF->addMissingImportedModule(missingImport);
  ctx.Diags.diagnose(loc, diag::missing_import_inserted, M->getName());
  return true;
}

bool TypeChecker::diagnoseInlinableDeclRefAccess(SourceLoc loc,
                                                 const ValueDecl *D,
                                                 const ExportContext &where) {
  auto fragileKind = where.getFragileFunctionKind();
  if (fragileKind.kind == FragileFunctionKind::None)
    return false;

  // Local declarations are OK.
  if (D->getDeclContext()->isLocalContext())
    return false;

  auto *DC = where.getDeclContext();
  auto &Context = DC->getASTContext();

  ImportAccessLevel problematicImport = D->getImportAccessFrom(DC);
  if (problematicImport.has_value()) {
    auto SF = DC->getParentSourceFile();
    if (SF)
      SF->registerAccessLevelUsingImport(problematicImport.value(),
                                         AccessLevel::Public);

    if (Context.LangOpts.EnableModuleApiImportRemarks) {
      ModuleDecl *importedVia = problematicImport->module.importedModule,
                 *sourceModule = D->getModuleContext();
      Context.Diags.diagnose(loc, diag::module_api_import,
                             D, importedVia, sourceModule,
                             importedVia == sourceModule,
                             /*isImplicit*/false);
    }
  }

  // General check on access-level of the decl.
  auto declAccessScope =
      D->getFormalAccessScope(/*useDC=*/DC,
                              /*allowUsableFromInline=*/true);

  // Public declarations are OK, even if they're SPI or came from an
  // implementation-only import. We'll diagnose exportability violations
  // from diagnoseDeclRefExportability().
  if (declAccessScope.isPublic())
    return false;

  // Dynamic declarations were mistakenly not checked in Swift 4.2.
  // Do enforce the restriction even in pre-Swift-5 modes if the module we're
  // building is resilient, though.
  if (D->shouldUseObjCDispatch() && !Context.isSwiftVersionAtLeast(5) &&
      !DC->getParentModule()->isResilient()) {
    return false;
  }

  DowngradeToWarning downgradeToWarning = DowngradeToWarning::No;

  // Swift 4.2 did not perform any checks for type aliases.
  if (isa<TypeAliasDecl>(D)) {
    if (!Context.isSwiftVersionAtLeast(4, 2))
      return false;
    if (!Context.isSwiftVersionAtLeast(5))
      downgradeToWarning = DowngradeToWarning::Yes;
  }

  // Swift 4.2 did not check accessor accessibility.
  if (auto accessor = dyn_cast<AccessorDecl>(D)) {
    if (!Context.isSwiftVersionAtLeast(5))
      downgradeToWarning = DowngradeToWarning::Yes;
  }

  // Swift 5.0 did not check the underlying types of local typealiases.
  if (isa<TypeAliasDecl>(DC) && !Context.isSwiftVersionAtLeast(6))
    downgradeToWarning = DowngradeToWarning::Yes;

  auto diagID = diag::resilience_decl_unavailable;
  if (downgradeToWarning == DowngradeToWarning::Yes)
    diagID = diag::resilience_decl_unavailable_warn;

  AccessLevel diagAccessLevel = declAccessScope.accessLevelForDiagnostics();
  Context.Diags.diagnose(loc, diagID, D, diagAccessLevel,
                         fragileKind.getSelector());

  Context.Diags.diagnose(D, diag::resilience_decl_declared_here, D);

  if (problematicImport.has_value() &&
      problematicImport->accessLevel < D->getFormalAccess()) {
    Context.Diags.diagnose(problematicImport->importLoc,
                           diag::decl_import_via_here, D,
                           problematicImport->accessLevel,
                           problematicImport->module.importedModule);
  }

  return (downgradeToWarning == DowngradeToWarning::No);
}

static bool diagnoseTypeAliasDeclRefExportability(SourceLoc loc,
                                                  const TypeAliasDecl *TAD,
                                                  const ExportContext &where) {
  assert(where.mustOnlyReferenceExportedDecls());

  auto *D = TAD->getUnderlyingType()->getAnyNominal();
  if (!D)
    return false;

  auto exportingModule = where.getDeclContext()->getParentModule();
  ASTContext &ctx = exportingModule->getASTContext();

  ImportAccessLevel problematicImport = D->getImportAccessFrom(
                                                       where.getDeclContext());
  if (problematicImport.has_value()) {
    auto SF = where.getDeclContext()->getParentSourceFile();
    if (SF)
      SF->registerAccessLevelUsingImport(problematicImport.value(),
                                         AccessLevel::Public);

    if (ctx.LangOpts.EnableModuleApiImportRemarks) {
      ModuleDecl *importedVia = problematicImport->module.importedModule,
                 *sourceModule = D->getModuleContext();
      ctx.Diags.diagnose(loc, diag::module_api_import_aliases,
                             D, importedVia, sourceModule,
                             importedVia == sourceModule);
    }
  }

  auto ignoredDowngradeToWarning = DowngradeToWarning::No;
  auto originKind =
      getDisallowedOriginKind(D, where, ignoredDowngradeToWarning);
  if (originKind == DisallowedOriginKind::None)
    return false;

  // As an exception, if the import of the module that defines the desugared
  // decl is just missing (as opposed to imported explicitly with reduced
  // visibility) then we should only diagnose if we're building a resilient
  // module.
  if (originKind == DisallowedOriginKind::MissingImport &&
      !exportingModule->isResilient())
    return false;

  auto definingModule = D->getModuleContext();
  auto fragileKind = where.getFragileFunctionKind();
  bool warnPreSwift6 = originKind != DisallowedOriginKind::SPIOnly &&
                       originKind != DisallowedOriginKind::NonPublicImport;
  if (fragileKind.kind == FragileFunctionKind::None) {
    auto reason = where.getExportabilityReason();
    ctx.Diags
        .diagnose(loc, diag::typealias_desugars_to_type_from_hidden_module,
                  TAD, definingModule->getNameStr(), D->getNameStr(),
                  static_cast<unsigned>(*reason), definingModule->getName(),
                  static_cast<unsigned>(originKind))
        .warnUntilSwiftVersionIf(warnPreSwift6, 6);
  } else {
    ctx.Diags
        .diagnose(loc,
                  diag::inlinable_typealias_desugars_to_type_from_hidden_module,
                  TAD, definingModule->getNameStr(), D->getNameStr(),
                  fragileKind.getSelector(), definingModule->getName(),
                  static_cast<unsigned>(originKind))
        .warnUntilSwiftVersionIf(warnPreSwift6, 6);
  }
  D->diagnose(diag::kind_declared_here, DescriptiveDeclKind::Type);

  if (originKind == DisallowedOriginKind::MissingImport &&
      !ctx.LangOpts.isSwiftVersionAtLeast(6))
    addMissingImport(loc, D, where);

  // If limited by an import, note which one.
  if (originKind == DisallowedOriginKind::NonPublicImport) {
    const DeclContext *DC = where.getDeclContext();
    ImportAccessLevel limitImport = D->getImportAccessFrom(DC);
    assert(limitImport.has_value() &&
           limitImport->accessLevel < AccessLevel::Public &&
           "The import should still be non-public");
    ctx.Diags.diagnose(limitImport->importLoc,
                       diag::decl_import_via_here, D,
                       limitImport->accessLevel,
                       limitImport->module.importedModule);
  }

  return true;
}

static bool diagnoseValueDeclRefExportability(SourceLoc loc, const ValueDecl *D,
                                              const ExportContext &where) {
  assert(where.mustOnlyReferenceExportedDecls());

  auto definingModule = D->getModuleContext();
  auto downgradeToWarning = DowngradeToWarning::No;

  auto reason = where.getExportabilityReason();
  auto DC = where.getDeclContext();
  ASTContext &ctx = DC->getASTContext();
  auto originKind = getDisallowedOriginKind(D, where, downgradeToWarning);

  // If we got here it was used in API, we can record the use of the import.
  ImportAccessLevel import = D->getImportAccessFrom(DC);
  if (import.has_value() && reason.has_value()) {
    auto SF = DC->getParentSourceFile();
    if (SF)
      SF->registerAccessLevelUsingImport(import.value(),
                                         AccessLevel::Public);
  }

  // Access levels from imports are reported with the others access levels.
  // Except for extensions and protocol conformances, we report them here.
  if (originKind == DisallowedOriginKind::NonPublicImport) {
    bool reportHere = [&] {
      switch (*reason) {
        case ExportabilityReason::ExtensionWithPublicMembers:
        case ExportabilityReason::ExtensionWithConditionalConformances:
          return true;
        case ExportabilityReason::Inheritance:
          return isa<ProtocolDecl>(D);
        default:
          return false;
      }
    }();
    if (!reportHere)
      return false;
  }

  if (ctx.LangOpts.EnableModuleApiImportRemarks &&
      import.has_value() && where.isExported() &&
      reason != ExportabilityReason::General &&
      originKind != DisallowedOriginKind::NonPublicImport) {
    // These may be reported twice, for the Type and for the TypeRepr.
    ModuleDecl *importedVia = import->module.importedModule,
               *sourceModule = D->getModuleContext();
    ctx.Diags.diagnose(loc, diag::module_api_import,
                       D, importedVia, sourceModule,
                       importedVia == sourceModule,
                       /*isImplicit*/false);
  }

  if (originKind == DisallowedOriginKind::None)
    return false;

  auto diagName = D->getName();
  if (auto accessor = dyn_cast<AccessorDecl>(D)) {
    // Only diagnose accessors if their disallowed origin kind differs from
    // that of their storage.
    if (getDisallowedOriginKind(accessor->getStorage(), where) == originKind)
      return false;

    // For accessors, diagnose with the name of the storage instead of the
    // implicit '_'.
    diagName = accessor->getStorage()->getName();
  }

  auto fragileKind = where.getFragileFunctionKind();
  if (fragileKind.kind == FragileFunctionKind::None) {
    DiagnosticBehavior limit = downgradeToWarning == DowngradeToWarning::Yes
                             ? DiagnosticBehavior::Warning
                             : DiagnosticBehavior::Unspecified;
    ctx.Diags.diagnose(loc, diag::decl_from_hidden_module, D,
                       static_cast<unsigned>(*reason),
                       definingModule->getName(),
                       static_cast<unsigned>(originKind))
        .limitBehavior(limit);

    D->diagnose(diag::kind_declared_here, DescriptiveDeclKind::Type);
  } else {
    // Only implicitly imported decls should be reported as a warning,
    // and only for language versions below Swift 6.
    assert(downgradeToWarning == DowngradeToWarning::No ||
           originKind == DisallowedOriginKind::MissingImport &&
           "Only implicitly imported decls should be reported as a warning.");

    ctx.Diags.diagnose(loc, diag::inlinable_decl_ref_from_hidden_module, D,
                       fragileKind.getSelector(), definingModule->getName(),
                       static_cast<unsigned>(originKind))
        .warnUntilSwiftVersionIf(downgradeToWarning == DowngradeToWarning::Yes,
                                 6);

    if (originKind == DisallowedOriginKind::MissingImport &&
        downgradeToWarning == DowngradeToWarning::Yes)
      addMissingImport(loc, D, where);
  }

  // If limited by an import, note which one.
  if (originKind == DisallowedOriginKind::NonPublicImport) {
    assert(import.has_value() &&
           import->accessLevel < AccessLevel::Public &&
           "The import should still be non-public");
    ctx.Diags.diagnose(import->importLoc,
                       diag::decl_import_via_here, D,
                       import->accessLevel,
                       import->module.importedModule);
  }

  return true;
}

bool TypeChecker::diagnoseDeclRefExportability(SourceLoc loc,
                                               const ValueDecl *D,
                                               const ExportContext &where) {
  if (!where.mustOnlyReferenceExportedDecls())
    return false;

  if (diagnoseValueDeclRefExportability(loc, D, where))
    return true;

  if (auto *TAD = dyn_cast<TypeAliasDecl>(D))
    if (diagnoseTypeAliasDeclRefExportability(loc, TAD, where))
      return true;

  return false;
}

bool
TypeChecker::diagnoseConformanceExportability(SourceLoc loc,
                                              const RootProtocolConformance *rootConf,
                                              const ExtensionDecl *ext,
                                              const ExportContext &where,
                                              bool warnIfConformanceUnavailablePreSwift6) {
  if (!where.mustOnlyReferenceExportedDecls())
    return false;

  // Skip the special Sendable and Copyable conformances we synthesized in
  // ASTContext::getBuiltinTupleDecl().
  if (ext->getParentModule()->isBuiltinModule())
    return false;

  ModuleDecl *M = ext->getParentModule();
  ASTContext &ctx = M->getASTContext();

  ImportAccessLevel problematicImport = ext->getImportAccessFrom(where.getDeclContext());
  if (problematicImport.has_value()) {
    auto SF = where.getDeclContext()->getParentSourceFile();
    if (SF)
      SF->registerAccessLevelUsingImport(problematicImport.value(),
                                         AccessLevel::Public);

    if (ctx.LangOpts.EnableModuleApiImportRemarks) {
      ModuleDecl *importedVia = problematicImport->module.importedModule,
                 *sourceModule = ext->getModuleContext();
      ctx.Diags.diagnose(loc, diag::module_api_import_conformance,
                         rootConf->getType(), rootConf->getProtocol(),
                         importedVia, sourceModule,
                         importedVia == sourceModule);
    }
  }

  auto originKind = getDisallowedOriginKind(ext, where);
  if (originKind == DisallowedOriginKind::None)
    return false;

  auto reason = where.getExportabilityReason();
  if (!reason.has_value())
    reason = ExportabilityReason::General;

  ctx.Diags.diagnose(loc, diag::conformance_from_implementation_only_module,
                     rootConf->getType(),
                     rootConf->getProtocol()->getName(),
                     static_cast<unsigned>(*reason),
                     M->getName(),
                     static_cast<unsigned>(originKind))
      .warnUntilSwiftVersionIf((warnIfConformanceUnavailablePreSwift6 &&
                                originKind != DisallowedOriginKind::SPIOnly &&
                                originKind != DisallowedOriginKind::NonPublicImport) ||
                               originKind == DisallowedOriginKind::MissingImport,
                               6);

  if (originKind == DisallowedOriginKind::MissingImport &&
      !ctx.LangOpts.isSwiftVersionAtLeast(6))
    addMissingImport(loc, ext, where);

  // If limited by an import, note which one.
  if (originKind == DisallowedOriginKind::NonPublicImport) {
    const DeclContext *DC = where.getDeclContext();
    ImportAccessLevel limitImport = ext->getImportAccessFrom(DC);
    assert(limitImport.has_value() &&
           limitImport->accessLevel < AccessLevel::Public &&
           "The import should still be non-public");
    ctx.Diags.diagnose(limitImport->importLoc,
                       diag::decl_import_via_here, ext,
                       limitImport->accessLevel,
                       limitImport->module.importedModule);
  }

  return true;
}
