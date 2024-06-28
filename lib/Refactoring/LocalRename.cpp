//===----------------------------------------------------------------------===//
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

#include "RefactoringActions.h"
#include "swift/AST/DiagnosticsRefactoring.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/USRGeneration.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/StringExtras.h"
#include "swift/Frontend/PrintingDiagnosticConsumer.h"
#include "swift/IDE/IDEBridging.h"
#include "swift/Index/Index.h"

using namespace swift::refactoring;
using namespace swift::index;

static const ValueDecl *getRelatedSystemDecl(const ValueDecl *VD) {
  if (VD->getModuleContext()->isNonUserModule())
    return VD;
  for (auto *Req : VD->getSatisfiedProtocolRequirements()) {
    if (Req->getModuleContext()->isNonUserModule())
      return Req;
  }
  for (auto Over = VD->getOverriddenDecl(); Over;
       Over = Over->getOverriddenDecl()) {
    if (Over->getModuleContext()->isNonUserModule())
      return Over;
  }
  return nullptr;
}

/// Stores information about the reference that rename availability is being
/// queried on.
struct RenameRefInfo {
  SourceFile *SF;  ///< The source file containing the reference.
  SourceLoc Loc;   ///< The reference's source location.
  bool IsArgLabel; ///< Whether Loc is on an arg label, rather than base name.
};

#if SWIFT_BUILD_SWIFT_SYNTAX
/// Returns `true` if the `RefInfo` points to a location that doesn't have any
/// arguments. For example, returns `true` for `Foo.init` but `false` for
/// `Foo.init()` or `Foo.init(a: 1)`.
static bool
isReferenceWithoutArguments(const std::optional<RenameRefInfo> &refInfo) {
  if (!refInfo) {
    return false;
  }
  if (refInfo->IsArgLabel) {
    return false;
  }
  std::vector<ResolvedLoc> resolvedLocs =
      runNameMatcher(*refInfo->SF, refInfo->Loc);
  if (!resolvedLocs.empty()) {
    ResolvedLoc resolvedLoc = resolvedLocs.front();
    return resolvedLoc.labelRanges.empty();
  }
  return false;
}
#endif // SWIFT_BUILD_SWIFT_SYNTAX

static std::optional<RefactorAvailabilityInfo>
renameAvailabilityInfo(const ValueDecl *VD,
                       std::optional<RenameRefInfo> RefInfo) {
  RefactorAvailableKind AvailKind = RefactorAvailableKind::Available;
  if (getRelatedSystemDecl(VD)) {
    AvailKind = RefactorAvailableKind::Unavailable_system_symbol;
  } else if (VD->getClangDecl()) {
    AvailKind = RefactorAvailableKind::Unavailable_decl_from_clang;
  } else if (!VD->hasName()) {
    AvailKind = RefactorAvailableKind::Unavailable_has_no_name;
  }

  auto isInMacroExpansionBuffer = [](const ValueDecl *VD) -> bool {
    auto *module = VD->getModuleContext();
    auto *file = module->getSourceFileContainingLocation(VD->getLoc());
    if (!file)
      return false;

    return file->getFulfilledMacroRole() != std::nullopt;
  };

  if (AvailKind == RefactorAvailableKind::Available) {
    SourceLoc Loc = VD->getLoc();
    if (!Loc.isValid()) {
      AvailKind = RefactorAvailableKind::Unavailable_has_no_location;
    } else if (isInMacroExpansionBuffer(VD)) {
      AvailKind = RefactorAvailableKind::Unavailable_decl_in_macro;
    }
  }

  if (isa<AbstractFunctionDecl>(VD)) {
    // Disallow renaming accessors.
    if (isa<AccessorDecl>(VD))
      return std::nullopt;

    // Disallow renaming deinit.
    if (isa<DestructorDecl>(VD))
      return std::nullopt;

    // Disallow renaming init with no arguments.
    if (auto CD = dyn_cast<ConstructorDecl>(VD)) {
      if (!CD->getParameters()->size())
        return std::nullopt;

#if SWIFT_BUILD_SWIFT_SYNTAX
      if (isReferenceWithoutArguments(RefInfo)) {
        return std::nullopt;
      }
#endif
    }

    // Disallow renaming 'callAsFunction' method with no arguments.
    if (auto FD = dyn_cast<FuncDecl>(VD)) {
      // FIXME: syntactic rename can only decide by checking the spelling, not
      // whether it's an instance method, so we do the same here for now.
      if (FD->getBaseIdentifier() == FD->getASTContext().Id_callAsFunction) {
        if (!FD->getParameters()->size())
          return std::nullopt;

#if SWIFT_BUILD_SWIFT_SYNTAX
        if (isReferenceWithoutArguments(RefInfo)) {
          return std::nullopt;
        }
#endif
      }
    }
  }

  // Always return local rename for parameters.
  // FIXME: if the cursor is on the argument, we should return global rename.
  if (isa<ParamDecl>(VD))
    return RefactorAvailabilityInfo{RefactoringKind::LocalRename, AvailKind};

  // If the indexer considers VD a global symbol, then we apply global rename.
  if (index::isLocalSymbol(VD))
    return RefactorAvailabilityInfo{RefactoringKind::LocalRename, AvailKind};
  return RefactorAvailabilityInfo{RefactoringKind::GlobalRename, AvailKind};
}

/// Given a cursor, return the decl and its rename availability. \c None if
/// the cursor did not resolve to a decl or it resolved to a decl that we do
/// not allow renaming on.
std::optional<RenameInfo>
swift::ide::getRenameInfo(ResolvedCursorInfoPtr cursorInfo) {
  auto valueCursor = dyn_cast<ResolvedValueRefCursorInfo>(cursorInfo);
  if (!valueCursor)
    return std::nullopt;

  ValueDecl *VD = valueCursor->typeOrValue();
  if (!VD)
    return std::nullopt;

  if (auto *V = dyn_cast<VarDecl>(VD)) {
    // Always use the canonical var decl for comparison. This is so we
    // pick up all occurrences of x in case statements like the below:
    //   case .first(let x), .second(let x)
    //     fallthrough
    //   case .third(let x)
    //     print(x)
    VD = V->getCanonicalVarDecl();

    // If we have a property wrapper backing property or projected value, use
    // the wrapped property instead (i.e. if this is _foo or $foo, pretend
    // it's foo).
    if (auto *Wrapped = V->getOriginalWrappedProperty()) {
      VD = Wrapped;
    }
  }

  std::optional<RenameRefInfo> refInfo;
  if (!valueCursor->getShorthandShadowedDecls().empty()) {
    // Find the outermost decl for a shorthand if let/closure capture
    VD = valueCursor->getShorthandShadowedDecls().back();
  } else if (valueCursor->isRef()) {
    refInfo = {valueCursor->getSourceFile(), valueCursor->getLoc(),
               valueCursor->isKeywordArgument()};
  }

  std::optional<RefactorAvailabilityInfo> info =
      renameAvailabilityInfo(VD, refInfo);
  if (!info)
    return std::nullopt;

  return RenameInfo{VD, *info};
}

class RenameRangeCollector : public IndexDataConsumer {
  const ValueDecl *declToRename;
  std::unique_ptr<StringScratchSpace> stringStorage;
  std::vector<RenameLoc> locations;

public:
  RenameRangeCollector(const ValueDecl *declToRename)
      : declToRename(declToRename), stringStorage(new StringScratchSpace()) {}

  RenameRangeCollector(RenameRangeCollector &&collector) = default;

  /// Take the resuls from the collector.
  /// This invalidates the collector and must only be called once.
  RenameLocs takeResults() && {
    return RenameLocs(locations, std::move(stringStorage));
  }

private:
  bool indexLocals() override { return true; }
  void failed(StringRef error) override {}
  bool startDependency(StringRef name, StringRef path, bool isClangModule,
                       bool isSystem) override {
    return true;
  }
  bool finishDependency(bool isClangModule) override { return true; }

  Action startSourceEntity(const IndexSymbol &symbol) override {
    if (symbol.decl != declToRename) {
      return IndexDataConsumer::Continue;
    }
    auto loc = indexSymbolToRenameLoc(symbol);
    if (!loc) {
      return IndexDataConsumer::Continue;
    }
    
    // Inside capture lists like `{ [test] in }`, 'test' refers to both the
    // newly declared, captured variable and the referenced variable it is
    // initialized from. Make sure to only rename it once.
    auto existingLoc = llvm::find_if(locations, [&](RenameLoc searchLoc) {
      return searchLoc.Line == loc->Line && searchLoc.Column == loc->Column;
    });
    if (existingLoc == locations.end()) {
      locations.push_back(std::move(*loc));
    } else {
      assert(existingLoc->OldName == loc->OldName &&
             "Asked to do a different rename for the same location?");
    }
    return IndexDataConsumer::Continue;
  }

  bool finishSourceEntity(SymbolInfo symInfo, SymbolRoleSet roles) override {
    return true;
  }

  std::optional<RenameLoc>
  indexSymbolToRenameLoc(const index::IndexSymbol &symbol);
};

std::optional<RenameLoc>
RenameRangeCollector::indexSymbolToRenameLoc(const index::IndexSymbol &symbol) {
  if (symbol.roles & (unsigned)index::SymbolRole::Implicit) {
    return std::nullopt;
  }

  RenameLocUsage usage = RenameLocUsage::Unknown;
  if (symbol.roles & (unsigned)index::SymbolRole::Call) {
    usage = RenameLocUsage::Call;
  } else if (symbol.roles & (unsigned)index::SymbolRole::Definition) {
    usage = RenameLocUsage::Definition;
  } else if (symbol.roles & (unsigned)index::SymbolRole::Reference) {
    usage = RenameLocUsage::Reference;
  } else {
    llvm_unreachable("unexpected role");
  }

  StringRef oldName = stringStorage->copyString(symbol.name);
  return RenameLoc{symbol.line, symbol.column, usage, oldName};
}

/// Get the decl context that we need to walk when renaming \p VD.
///
/// This \c DeclContext contains all possible references to \c VD within the
/// file.
DeclContext *getRenameScope(const ValueDecl *VD) {
  auto *Scope = VD->getDeclContext();
  // There may be sibling decls that the renamed symbol is visible from.
  switch (Scope->getContextKind()) {
  case DeclContextKind::GenericTypeDecl:
  case DeclContextKind::ExtensionDecl:
  case DeclContextKind::TopLevelCodeDecl:
  case DeclContextKind::SubscriptDecl:
  case DeclContextKind::EnumElementDecl:
  case DeclContextKind::AbstractFunctionDecl:
    Scope = Scope->getParent();
    break;
  case DeclContextKind::AbstractClosureExpr:
  case DeclContextKind::SerializedAbstractClosure:
  case DeclContextKind::SerializedTopLevelCodeDecl:
  case DeclContextKind::Initializer:
  case DeclContextKind::Package:
  case DeclContextKind::Module:
  case DeclContextKind::FileUnit:
  case DeclContextKind::MacroDecl:
    break;
  }

  return Scope;
}

/// Get the declaration at `startLoc` and validate that we can perform local
/// rename on it (e.g. checking that the original definition isn't a system
/// symbol).
///
/// If the validation succeeds, return the `ValueDecl`, otherwise add an error
/// to `diags` and return `nullptr`.
static ValueDecl *getDeclForLocalRename(SourceFile *sourceFile,
                                        SourceLoc startLoc,
                                        DiagnosticEngine &diags) {
  auto cursorInfo = evaluateOrDefault(
      sourceFile->getASTContext().evaluator,
      CursorInfoRequest{CursorInfoOwner(sourceFile, startLoc)},
      new ResolvedCursorInfo());

  std::optional<RenameInfo> info = getRenameInfo(cursorInfo);
  if (!info) {
    diags.diagnose(startLoc, diag::unresolved_location);
    return nullptr;
  }

  switch (info->Availability.AvailableKind) {
  case RefactorAvailableKind::Available:
    break;
  case RefactorAvailableKind::Unavailable_system_symbol:
    diags.diagnose(startLoc, diag::decl_is_system_symbol, info->VD->getName());
    return nullptr;
  case RefactorAvailableKind::Unavailable_has_no_location:
    diags.diagnose(startLoc, diag::value_decl_no_loc, info->VD->getName());
    return nullptr;
  case RefactorAvailableKind::Unavailable_has_no_name:
    diags.diagnose(startLoc, diag::decl_has_no_name);
    return nullptr;
  case RefactorAvailableKind::Unavailable_has_no_accessibility:
    diags.diagnose(startLoc, diag::decl_no_accessibility);
    return nullptr;
  case RefactorAvailableKind::Unavailable_decl_from_clang:
    diags.diagnose(startLoc, diag::decl_from_clang);
    return nullptr;
  case RefactorAvailableKind::Unavailable_decl_in_macro:
    diags.diagnose(startLoc, diag::decl_in_macro);
    return nullptr;
  }

  return info->VD;
}

RenameLocs swift::ide::localRenameLocs(SourceFile *SF,
                                       const ValueDecl *valueDecl) {
  DeclContext *RenameScope = SF;
  if (RenameScope) {
    // If the value is declared in a DeclContext that's a child of the file in
    // which we are performing the rename, we can limit our analysis to this
    // decl context.
    //
    // Cases where the rename scope is not a child of the source file include
    // if we are getting related identifiers of a type A that is defined in
    // another file. In this case, we need to analyze the entire file.
    auto DeclarationScope = getRenameScope(valueDecl);
    if (DeclarationScope->isChildContextOf(SF)) {
      RenameScope = DeclarationScope;
    }
  }

  RenameRangeCollector rangeCollector(valueDecl);
  indexDeclContext(RenameScope, rangeCollector);

  return std::move(rangeCollector).takeResults();
}

CancellableResult<std::vector<SyntacticRenameRangeDetails>>
swift::ide::findLocalRenameRanges(SourceFile *SF, RangeConfig Range) {
  using ResultType =
      CancellableResult<std::vector<SyntacticRenameRangeDetails>>;
  assert(SF && "null source file");

  SourceManager &SM = SF->getASTContext().SourceMgr;
  std::string ErrBuffer;
  llvm::raw_string_ostream DiagOS(ErrBuffer);
  swift::PrintingDiagnosticConsumer DiagConsumer(DiagOS);
  DiagnosticEngine Diags(SM);
  Diags.addConsumer(DiagConsumer);

  auto StartLoc = Lexer::getLocForStartOfToken(SM, Range.getStart(SM));
  ValueDecl *declToRename = getDeclForLocalRename(SF, StartLoc, Diags);
  if (!declToRename || DiagConsumer.didErrorOccur()) {
    return ResultType::failure(ErrBuffer);
  }

  RenameLocs RenameRanges = localRenameLocs(SF, declToRename);

  return findSyntacticRenameRanges(SF, RenameRanges.getLocations(),
                                   /*NewName=*/StringRef());
}
