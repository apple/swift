//===--- ASTBridging.cpp - AST bridging functions -------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ASTBridging.h"

#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTNode.h"
#include "swift/AST/Decl.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/DiagnosticsCommon.h"
#include "swift/AST/Expr.h"
#include "swift/AST/GenericParamList.h"
#include "swift/AST/Identifier.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/ParseRequests.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/PluginRegistry.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/TypeRepr.h"
#include "swift/Basic/Assertions.h"

#ifdef PURE_BRIDGING_MODE
// In PURE_BRIDGING_MODE, bridging functions are not inlined and therefore
// inluded in the cpp file.
#include "swift/AST/ASTBridgingImpl.h"
#endif

using namespace swift;

static StaticSpellingKind unbridged(BridgedStaticSpelling kind) {
  return static_cast<StaticSpellingKind>(kind);
}

static AccessorKind unbridged(BridgedAccessorKind kind) {
  return static_cast<AccessorKind>(kind);
}

//===----------------------------------------------------------------------===//
// MARK: Identifier
//===----------------------------------------------------------------------===//

BridgedDeclBaseName BridgedDeclBaseName_createConstructor() {
  return DeclBaseName::createConstructor();
}

BridgedDeclBaseName BridgedDeclBaseName_createDestructor() {
  return DeclBaseName::createDestructor();
}

BridgedDeclBaseName BridgedDeclBaseName_createSubscript() {
  return DeclBaseName::createSubscript();
}

BridgedDeclBaseName
BridgedDeclBaseName_createIdentifier(BridgedIdentifier identifier) {
  return DeclBaseName(identifier.unbridged());
}

BridgedDeclNameRef
BridgedDeclNameRef_createParsed(BridgedASTContext cContext,
                                BridgedDeclBaseName cBaseName,
                                BridgedArrayRef cLabels) {
  ASTContext &context = cContext.unbridged();
  SmallVector<Identifier, 4> labels;
  for (auto &cLabel : cLabels.unbridged<BridgedIdentifier>()) {
    labels.push_back(cLabel.unbridged());
  }
  return DeclNameRef(DeclName(context, cBaseName.unbridged(), labels));
}

BridgedDeclNameRef
BridgedDeclNameRef_createParsed(BridgedDeclBaseName cBaseName) {
  return DeclNameRef(cBaseName.unbridged());
}

BridgedDeclNameLoc BridgedDeclNameLoc_createParsed(
    BridgedASTContext cContext, BridgedSourceLoc cBaseNameLoc,
    BridgedSourceLoc cLParenLoc, BridgedArrayRef cLabelLocs,
    BridgedSourceLoc cRParenLoc) {

  ASTContext &context = cContext.unbridged();
  SmallVector<SourceLoc, 4> labelLocs;
  for (auto &cLabelLoc : cLabelLocs.unbridged<BridgedSourceLoc>())
    labelLocs.push_back(cLabelLoc.unbridged());

  return DeclNameLoc(context, cBaseNameLoc.unbridged(), cLParenLoc.unbridged(),
                     labelLocs, cRParenLoc.unbridged());
}

BridgedDeclNameLoc
BridgedDeclNameLoc_createParsed(BridgedSourceLoc cBaseNameLoc) {
  return DeclNameLoc(cBaseNameLoc.unbridged());
}

//===----------------------------------------------------------------------===//
// MARK: ASTContext
//===----------------------------------------------------------------------===//

BridgedIdentifier BridgedASTContext_getIdentifier(BridgedASTContext cContext,
                                                  BridgedStringRef cStr) {
  return cContext.unbridged().getIdentifier(cStr.unbridged());
}

bool BridgedASTContext_langOptsHasFeature(BridgedASTContext cContext,
                                          BridgedFeature feature) {
  return cContext.unbridged().LangOpts.hasFeature((Feature)feature);
}

unsigned BridgedASTContext_majorLanguageVersion(BridgedASTContext cContext) {
  return cContext.unbridged().LangOpts.EffectiveLanguageVersion[0];
}

//===----------------------------------------------------------------------===//
// MARK: AST nodes
//===----------------------------------------------------------------------===//

// Define `.asDecl` on each BridgedXXXDecl type.
#define DECL(Id, Parent)                                                       \
  BridgedDecl Bridged##Id##Decl_asDecl(Bridged##Id##Decl decl) {               \
    return static_cast<Decl *>(decl.unbridged());                              \
  }
#define ABSTRACT_DECL(Id, Parent) DECL(Id, Parent)
#include "swift/AST/DeclNodes.def"

// Define `.asDeclContext` on each BridgedXXXDecl type that's also a
// DeclContext.
#define DECL(Id, Parent)
#define CONTEXT_DECL(Id, Parent)                                               \
  BridgedDeclContext Bridged##Id##Decl_asDeclContext(Bridged##Id##Decl decl) { \
    return static_cast<DeclContext *>(decl.unbridged());                       \
  }
#define ABSTRACT_CONTEXT_DECL(Id, Parent) CONTEXT_DECL(Id, Parent)
#include "swift/AST/DeclNodes.def"

// Define `.asStmt` on each BridgedXXXStmt type.
#define STMT(Id, Parent)                                                       \
  BridgedStmt Bridged##Id##Stmt_asStmt(Bridged##Id##Stmt stmt) {               \
    return static_cast<Stmt *>(stmt.unbridged());                              \
  }
#define ABSTRACT_STMT(Id, Parent) STMT(Id, Parent)
#include "swift/AST/StmtNodes.def"

// Define `.asExpr` on each BridgedXXXExpr type.
#define EXPR(Id, Parent)                                                       \
  BridgedExpr Bridged##Id##Expr_asExpr(Bridged##Id##Expr expr) {               \
    return static_cast<Expr *>(expr.unbridged());                              \
  }
#define ABSTRACT_EXPR(Id, Parent) EXPR(Id, Parent)
#include "swift/AST/ExprNodes.def"

// Define `.asTypeRepr` on each BridgedXXXTypeRepr type.
#define TYPEREPR(Id, Parent)                                                   \
  BridgedTypeRepr Bridged##Id##TypeRepr_asTypeRepr(                            \
      Bridged##Id##TypeRepr typeRepr) {                                        \
    return static_cast<TypeRepr *>(typeRepr.unbridged());                      \
  }
#define ABSTRACT_TYPEREPR(Id, Parent) TYPEREPR(Id, Parent)
#include "swift/AST/TypeReprNodes.def"

// Define `.asPattern` on each BridgedXXXPattern type.
#define PATTERN(Id, Parent)                                                    \
  BridgedPattern Bridged##Id##Pattern_asPattern(                               \
      Bridged##Id##Pattern pattern) {                                          \
    return static_cast<Pattern *>(pattern.unbridged());                        \
  }
#include "swift/AST/PatternNodes.def"

#define SIMPLE_DECL_ATTR(...)
#define DECL_ATTR(_, CLASS, ...)                                               \
  BridgedDeclAttribute Bridged##CLASS##Attr_asDeclAttribute(                   \
      Bridged##CLASS##Attr attr) {                                             \
    return static_cast<DeclAttribute *>(attr.unbridged());                     \
  }
#include "swift/AST/DeclAttr.def"

//===----------------------------------------------------------------------===//
// MARK: Diagnostics
//===----------------------------------------------------------------------===//

static_assert(sizeof(BridgedDiagnosticArgument) >= sizeof(DiagnosticArgument),
              "BridgedDiagnosticArgument has wrong size");

BridgedDiagnosticArgument::BridgedDiagnosticArgument(SwiftInt i)
    : BridgedDiagnosticArgument(DiagnosticArgument((int)i)) {}

BridgedDiagnosticArgument::BridgedDiagnosticArgument(BridgedStringRef s)
    : BridgedDiagnosticArgument(DiagnosticArgument(s.unbridged())) {}

static_assert(sizeof(BridgedDiagnosticFixIt) >= sizeof(DiagnosticInfo::FixIt),
              "BridgedDiagnosticFixIt has wrong size");

BridgedDiagnosticFixIt::BridgedDiagnosticFixIt(BridgedSourceLoc start,
                                               uint32_t length,
                                               BridgedStringRef text)
    : BridgedDiagnosticFixIt(DiagnosticInfo::FixIt(
          CharSourceRange(start.unbridged(), length), text.unbridged(),
          llvm::ArrayRef<DiagnosticArgument>())) {}

void BridgedDiagnosticEngine_diagnose(
    BridgedDiagnosticEngine bridgedEngine, BridgedSourceLoc loc,
    BridgedDiagID bridgedDiagID,
    BridgedArrayRef /*BridgedDiagnosticArgument*/ bridgedArguments,
    BridgedSourceLoc highlightStart, uint32_t hightlightLength,
    BridgedArrayRef /*BridgedDiagnosticFixIt*/ bridgedFixIts) {
  auto *D = bridgedEngine.unbridged();

  auto diagID = static_cast<DiagID>(bridgedDiagID);
  SmallVector<DiagnosticArgument, 2> arguments;
  for (auto arg : bridgedArguments.unbridged<BridgedDiagnosticArgument>()) {
    arguments.push_back(arg.unbridged());
  }
  auto inflight = D->diagnose(loc.unbridged(), diagID, arguments);

  // Add highlight.
  if (highlightStart.unbridged().isValid()) {
    CharSourceRange highlight(highlightStart.unbridged(),
                              (unsigned)hightlightLength);
    inflight.highlightChars(highlight.getStart(), highlight.getEnd());
  }

  // Add fix-its.
  for (const BridgedDiagnosticFixIt &fixIt :
       bridgedFixIts.unbridged<BridgedDiagnosticFixIt>()) {
    auto range = fixIt.unbridged().getRange();
    auto text = fixIt.unbridged().getText();
    inflight.fixItReplaceChars(range.getStart(), range.getEnd(), text);
  }
}

bool BridgedDiagnosticEngine_hadAnyError(
    BridgedDiagnosticEngine bridgedEngine) {
  return bridgedEngine.unbridged()->hadAnyError();
}

struct BridgedDiagnostic::Impl {
  typedef llvm::MallocAllocator Allocator;

  InFlightDiagnostic inFlight;
  std::vector<StringRef> textBlobs;

  Impl(InFlightDiagnostic inFlight, std::vector<StringRef> textBlobs)
      : inFlight(std::move(inFlight)), textBlobs(std::move(textBlobs)) {}

  Impl(const Impl &) = delete;
  Impl(Impl &&) = delete;
  Impl &operator=(const Impl &) = delete;
  Impl &operator=(Impl &&) = delete;

  ~Impl() {
    inFlight.flush();

    Allocator allocator;
    for (auto text : textBlobs) {
      allocator.Deallocate(text.data(), text.size());
    }
  }
};

BridgedDiagnostic BridgedDiagnostic_create(BridgedSourceLoc cLoc,
                                           BridgedStringRef cText,
                                           BridgedDiagnosticSeverity severity,
                                           BridgedDiagnosticEngine cDiags) {
  StringRef origText = cText.unbridged();
  BridgedDiagnostic::Impl::Allocator alloc;
  StringRef text = origText.copy(alloc);

  SourceLoc loc = cLoc.unbridged();

  Diag<StringRef> diagID;
  switch (severity) {
  case BridgedDiagnosticSeverity::BridgedError:
    diagID = diag::bridged_error;
    break;
  case BridgedDiagnosticSeverity::BridgedFatalError:
    diagID = diag::bridged_fatal_error;
    break;
  case BridgedDiagnosticSeverity::BridgedNote:
    diagID = diag::bridged_note;
    break;
  case BridgedDiagnosticSeverity::BridgedRemark:
    diagID = diag::bridged_remark;
    break;
  case BridgedDiagnosticSeverity::BridgedWarning:
    diagID = diag::bridged_warning;
    break;
  }

  DiagnosticEngine &diags = *cDiags.unbridged();
  return new BridgedDiagnostic::Impl{diags.diagnose(loc, diagID, text), {text}};
}

/// Highlight a source range as part of the diagnostic.
void BridgedDiagnostic_highlight(BridgedDiagnostic cDiag,
                                 BridgedSourceLoc cStartLoc,
                                 BridgedSourceLoc cEndLoc) {
  SourceLoc startLoc = cStartLoc.unbridged();
  SourceLoc endLoc = cEndLoc.unbridged();

  BridgedDiagnostic::Impl *diag = cDiag.unbridged();
  diag->inFlight.highlightChars(startLoc, endLoc);
}

/// Add a Fix-It to replace a source range as part of the diagnostic.
void BridgedDiagnostic_fixItReplace(BridgedDiagnostic cDiag,
                                    BridgedSourceLoc cStartLoc,
                                    BridgedSourceLoc cEndLoc,
                                    BridgedStringRef cReplaceText) {

  SourceLoc startLoc = cStartLoc.unbridged();
  SourceLoc endLoc = cEndLoc.unbridged();

  StringRef origReplaceText = cReplaceText.unbridged();
  BridgedDiagnostic::Impl::Allocator alloc;
  StringRef replaceText = origReplaceText.copy(alloc);

  BridgedDiagnostic::Impl *diag = cDiag.unbridged();
  diag->textBlobs.push_back(replaceText);
  diag->inFlight.fixItReplaceChars(startLoc, endLoc, replaceText);
}

/// Finish the given diagnostic and emit it.
void BridgedDiagnostic_finish(BridgedDiagnostic cDiag) {
  BridgedDiagnostic::Impl *diag = cDiag.unbridged();
  delete diag;
}

//===----------------------------------------------------------------------===//
// MARK: DeclContexts
//===----------------------------------------------------------------------===//

bool BridgedDeclContext_isLocalContext(BridgedDeclContext cDeclContext) {
  return cDeclContext.unbridged()->isLocalContext();
}

BridgedPatternBindingInitializer
BridgedPatternBindingInitializer_create(BridgedDeclContext cDeclContext) {
  return PatternBindingInitializer::create(cDeclContext.unbridged());
}

BridgedDeclContext BridgedPatternBindingInitializer_asDeclContext(
    BridgedPatternBindingInitializer cInit) {
  return cInit.unbridged();
}
//===----------------------------------------------------------------------===//
// MARK: DeclAttributes
//===----------------------------------------------------------------------===//

BridgedDeclAttrKind BridgedDeclAttrKind_fromString(BridgedStringRef cStr) {
  auto optKind = DeclAttribute::getAttrKindFromString(cStr.unbridged());
  if (!optKind)
    return BridgedDeclAttrKindNone;
  switch (*optKind) {
#define DECL_ATTR(_, CLASS, ...)                                               \
  case DeclAttrKind::CLASS:                                                    \
    return BridgedDeclAttrKind##CLASS;
#include "swift/AST/DeclAttr.def"
  }
}

std::optional<DeclAttrKind> unbridged(BridgedDeclAttrKind kind) {
  switch (kind) {
#define DECL_ATTR(_, CLASS, ...)                                               \
  case BridgedDeclAttrKind##CLASS:                                             \
    return DeclAttrKind::CLASS;
#include "swift/AST/DeclAttr.def"
  case BridgedDeclAttrKindNone:
    return std::nullopt;
  }
  llvm_unreachable("unhandled enum value");
}

BridgedDeclAttribute BridgedDeclAttribute_createSimple(
    BridgedASTContext cContext, BridgedDeclAttrKind cKind,
    BridgedSourceLoc cAtLoc, BridgedSourceLoc cAttrLoc) {
  auto optKind = unbridged(cKind);
  assert(optKind && "creating attribute of invalid kind?");
  return DeclAttribute::createSimple(cContext.unbridged(), *optKind,
                                     cAtLoc.unbridged(), cAttrLoc.unbridged());
}

void BridgedDeclAttributes_add(BridgedDeclAttributes *cAttrs,
                               BridgedDeclAttribute cAdd) {
  auto attrs = cAttrs->unbridged();
  attrs.add(cAdd.unbridged());
  *cAttrs = attrs;
}

static AccessLevel unbridged(BridgedAccessLevel level) {
  switch (level) {
  case BridgedAccessLevelPrivate:
    return AccessLevel::Private;
  case BridgedAccessLevelFilePrivate:
    return AccessLevel::FilePrivate;
  case BridgedAccessLevelInternal:
    return AccessLevel::Internal;
  case BridgedAccessLevelPackage:
    return AccessLevel::Package;
  case BridgedAccessLevelPublic:
    return AccessLevel::Public;
  case BridgedAccessLevelOpen:
    return AccessLevel::Open;
  }
  llvm_unreachable("unhandled BridgedAccessLevel");
}

BridgedAccessControlAttr
BridgedAccessControlAttr_createParsed(BridgedASTContext cContext,
                                      BridgedSourceRange cRange,
                                      BridgedAccessLevel cAccessLevel) {
  return new (cContext.unbridged()) AccessControlAttr(
      /*atLoc=*/{}, cRange.unbridged(), unbridged(cAccessLevel));
}

BridgedAlignmentAttr
BridgedAlignmentAttr_createParsed(BridgedASTContext cContext,
                                  BridgedSourceLoc cAtLoc,
                                  BridgedSourceRange cRange, size_t cValue) {
  return new (cContext.unbridged()) AlignmentAttr(
      cValue, cAtLoc.unbridged(), cRange.unbridged(), /*Implicit=*/false);
}

BridgedAllowFeatureSuppressionAttr
BridgedAllowFeatureSuppressionAttr_createParsed(BridgedASTContext cContext,
                                                BridgedSourceLoc cAtLoc,
                                                BridgedSourceRange cRange,
                                                bool inverted,
                                                BridgedArrayRef cFeatures) {
  SmallVector<Identifier> features;
  for (auto elem : cFeatures.unbridged<BridgedIdentifier>())
    features.push_back(elem.unbridged());
  return AllowFeatureSuppressionAttr::create(
      cContext.unbridged(), cAtLoc.unbridged(), cRange.unbridged(),
      /*implicit*/ false, inverted, features);
}

BridgedCDeclAttr BridgedCDeclAttr_createParsed(BridgedASTContext cContext,
                                               BridgedSourceLoc cAtLoc,
                                               BridgedSourceRange cRange,
                                               BridgedStringRef cName) {
  return new (cContext.unbridged())
      CDeclAttr(cName.unbridged(), cAtLoc.unbridged(), cRange.unbridged(),
                /*Implicit=*/false);
}

BridgedDynamicReplacementAttr BridgedDynamicReplacementAttr_createParsed(
    BridgedASTContext cContext, BridgedSourceLoc cAtLoc,
    BridgedSourceLoc cAttrNameLoc, BridgedSourceLoc cLParenLoc,
    BridgedDeclNameRef cReplacedFunction, BridgedSourceLoc cRParenLoc) {
  return DynamicReplacementAttr::create(
      cContext.unbridged(), cAtLoc.unbridged(), cAttrNameLoc.unbridged(),
      cLParenLoc.unbridged(), cReplacedFunction.unbridged(),
      cRParenLoc.unbridged());
}

static EffectsKind unbridged(BridgedEffectsKind kind) {
  switch (kind) {
  case BridgedEffectsKindReadNone:
    return EffectsKind::ReadNone;
  case BridgedEffectsKindReadOnly:
    return EffectsKind::ReadOnly;
  case BridgedEffectsKindReleaseNone:
    return EffectsKind::ReleaseNone;
  case BridgedEffectsKindReadWrite:
    return EffectsKind::ReadWrite;
  case BridgedEffectsKindUnspecified:
    return EffectsKind::Unspecified;
  case BridgedEffectsKindCustom:
    return EffectsKind::Custom;
  }
  llvm_unreachable("unhandled kind");
}

BridgedEffectsAttr BridgedEffectsAttr_createParsed(
    BridgedASTContext cContext, BridgedSourceLoc cAtLoc,
    BridgedSourceRange cRange, BridgedEffectsKind cEffectKind) {
  return new (cContext.unbridged()) EffectsAttr(
      cAtLoc.unbridged(), cRange.unbridged(), unbridged(cEffectKind));
}

BridgedEffectsAttr BridgedEffectsAttr_createParsed(
    BridgedASTContext cContext, BridgedSourceLoc cAtLoc,
    BridgedSourceRange cRange, BridgedStringRef cCustomString,
    BridgedSourceLoc cCustomStringLoc) {
  return new (cContext.unbridged())
      EffectsAttr(cAtLoc.unbridged(), cRange.unbridged(),
                  cCustomString.unbridged(), cCustomStringLoc.unbridged());
}

static ExclusivityAttr::Mode unbridged(BridgedExclusivityAttrMode mode) {
  switch (mode) {
  case BridgedExclusivityAttrModeChecked:
    return ExclusivityAttr::Checked;
  case BridgedExclusivityAttrModeUnchecked:
    return ExclusivityAttr::Unchecked;
  }
  llvm_unreachable("unhandled enum value");
}
BridgedExclusivityAttr BridgedExclusivityAttr_createParsed(
    BridgedASTContext cContext, BridgedSourceLoc cAtLoc,
    BridgedSourceRange cRange, BridgedExclusivityAttrMode cMode) {
  return new (cContext.unbridged())
      ExclusivityAttr(cAtLoc.unbridged(), cRange.unbridged(), unbridged(cMode));
}

static ExposureKind unbridged(BridgedExposureKind kind) {
  switch (kind) {
  case BridgedExposureKindCxx:
    return ExposureKind::Cxx;
  case BridgedExposureKindWasm:
    return ExposureKind::Wasm;
  }
  llvm_unreachable("unhandled enum value");
}

BridgedExposeAttr BridgedExposeAttr_createParsed(BridgedASTContext cContext,
                                                 BridgedSourceLoc cAtLoc,
                                                 BridgedSourceRange cRange,
                                                 BridgedStringRef cName,
                                                 BridgedExposureKind cKind) {
  return new (cContext.unbridged())
      ExposeAttr(cName.unbridged(), cAtLoc.unbridged(), cRange.unbridged(),
                 unbridged(cKind), /*Implicit=*/false);
}

static ExternKind unbridged(BridgedExternKind kind) {
  switch (kind) {
  case BridgedExternKindC:
    return ExternKind::C;
  case BridgedExternKindWasm:
    return ExternKind::Wasm;
  }
  llvm_unreachable("unhandled enum value");
}

BridgedExternAttr BridgedExternAttr_createParsed(
    BridgedASTContext cContext, BridgedSourceLoc cAtLoc,
    BridgedSourceRange cRange, BridgedSourceLoc cLParenLoc,
    BridgedSourceLoc cRParenLoc, BridgedExternKind cKind,
    BridgedStringRef cModuleName, BridgedStringRef cName) {
  std::optional<StringRef> moduleName = cModuleName.unbridged();
  if (moduleName->empty())
    moduleName = std::nullopt;

  std::optional<StringRef> name = cName.unbridged();
  if (name->empty())
    name = std::nullopt;

  return new (cContext.unbridged())
      ExternAttr(moduleName, name, cAtLoc.unbridged(), cLParenLoc.unbridged(),
                 cRParenLoc.unbridged(), cRange.unbridged(), unbridged(cKind),
                 /*Implicit=*/false);
}

BridgedImplementsAttr BridgedImplementsAttr_createParsed(
    BridgedASTContext cContext, BridgedSourceLoc cAtLoc,
    BridgedSourceRange cRange, BridgedTypeRepr cProtocolType,
    BridgedDeclNameRef cMemberName, BridgedDeclNameLoc cMemberNameLoc) {
  return ImplementsAttr::create(cContext.unbridged(), cAtLoc.unbridged(),
                                cRange.unbridged(), cProtocolType.unbridged(),
                                cMemberName.unbridged().getFullName(),
                                cMemberNameLoc.unbridged());
}

static InlineKind unbridged(BridgedInlineKind kind) {
  switch (kind) {
  case BridgedInlineKindNever:
    return InlineKind::Never;
  case BridgedInlineKindAlways:
    return InlineKind::Always;
  }
  llvm_unreachable("unhandled enum value");
}

BridgedInlineAttr BridgedInlineAttr_createParsed(BridgedASTContext cContext,
                                                 BridgedSourceLoc cAtLoc,
                                                 BridgedSourceRange cRange,
                                                 BridgedInlineKind cKind) {
  return new (cContext.unbridged())
      InlineAttr(cAtLoc.unbridged(), cRange.unbridged(), unbridged(cKind));
}

BridgedMainTypeAttr
BridgedMainTypeAttr_createParsed(BridgedASTContext cContext,
                                 BridgedSourceLoc cAtLoc,
                                 BridgedSourceLoc cNameLoc) {
  return new (cContext.unbridged())
      MainTypeAttr(cAtLoc.unbridged(), cNameLoc.unbridged());
}

BridgedSwiftNativeObjCRuntimeBaseAttr
BridgedSwiftNativeObjCRuntimeBaseAttr_createParsed(BridgedASTContext cContext,
                                                   BridgedSourceLoc cAtLoc,
                                                   BridgedSourceRange cRange,
                                                   BridgedIdentifier cName) {
  return new (cContext.unbridged())
      SwiftNativeObjCRuntimeBaseAttr(cName.unbridged(), cAtLoc.unbridged(),
                                     cRange.unbridged(), /*Implicit=*/false);
}

static NonSendableKind unbridged(BridgedNonSendableKind kind) {
  switch (kind) {
  case BridgedNonSendableKindSpecific:
    return NonSendableKind::Specific;
  case BridgedNonSendableKindAssumed:
    return NonSendableKind::Assumed;
  }
  llvm_unreachable("unhandled enum value");
}

BridgedNonSendableAttr BridgedNonSendableAttr_createParsed(
    BridgedASTContext cContext, BridgedSourceLoc cAtLoc,
    BridgedSourceRange cRange, BridgedNonSendableKind cKind) {
  return new (cContext.unbridged())
      NonSendableAttr(cAtLoc.unbridged(), cRange.unbridged(), unbridged(cKind));
}

BridgedObjCAttr
BridgedObjCAttr_createParsedUnnamed(BridgedASTContext cContext,
                                    BridgedSourceLoc cAtLoc,
                                    BridgedSourceLoc cAttrNameLoc) {
  return ObjCAttr::createUnnamed(cContext.unbridged(), cAtLoc.unbridged(),
                                 cAttrNameLoc.unbridged());
}

BridgedObjCAttr BridgedObjCAttr_createParsedNullary(
    BridgedASTContext cContext, BridgedSourceLoc cAtLoc,
    BridgedSourceLoc cAttrNameLoc, BridgedSourceLoc cLParenLoc,
    BridgedSourceLoc cNameLoc, BridgedIdentifier cName,
    BridgedSourceLoc cRParenLoc) {
  return ObjCAttr::createNullary(cContext.unbridged(), cAtLoc.unbridged(),
                                 cAttrNameLoc.unbridged(),
                                 cLParenLoc.unbridged(), cNameLoc.unbridged(),
                                 cName.unbridged(), cRParenLoc.unbridged());
}

BridgedObjCAttr BridgedObjCAttr_createParsedSelector(
    BridgedASTContext cContext, BridgedSourceLoc cAtLoc,
    BridgedSourceLoc cAttrNameLoc, BridgedSourceLoc cLParenLoc,
    BridgedArrayRef cNameLocs, BridgedArrayRef cNames,
    BridgedSourceLoc cRParenLoc) {
  SmallVector<SourceLoc> nameLocs;
  for (auto elem : cNameLocs.unbridged<BridgedSourceLoc>())
    nameLocs.push_back(elem.unbridged());
  SmallVector<Identifier> names;
  for (auto elem : cNames.unbridged<BridgedIdentifier>())
    names.push_back(elem.unbridged());

  return ObjCAttr::createSelector(
      cContext.unbridged(), cAtLoc.unbridged(), cAttrNameLoc.unbridged(),
      cLParenLoc.unbridged(), nameLocs, names, cLParenLoc.unbridged());
}

BridgedObjCImplementationAttr BridgedObjCImplementationAttr_createParsed(
    BridgedASTContext cContext, BridgedSourceLoc cAtLoc,
    BridgedSourceRange cRange, BridgedIdentifier cName, bool isEarlyAdopter) {
  return new (cContext.unbridged()) ObjCImplementationAttr(
      cName.unbridged(), cAtLoc.unbridged(), cRange.unbridged(),
      isEarlyAdopter);
}

BridgedObjCRuntimeNameAttr BridgedObjCRuntimeNameAttr_createParsed(
    BridgedASTContext cContext, BridgedSourceLoc cAtLoc,
    BridgedSourceRange cRange, BridgedIdentifier cName) {
  return new (cContext.unbridged())
      ObjCRuntimeNameAttr(cName.unbridged().str(), cAtLoc.unbridged(),
                          cRange.unbridged(), /*Implicit=*/false);
}

static OptimizationMode unbridged(BridgedOptimizationMode mode) {
  switch (mode) {
  case BridgedOptimizationModeForSpeed:
    return OptimizationMode::ForSpeed;
  case BridgedOptimizationModeForSize:
    return OptimizationMode::ForSize;
  case BridgedOptimizationModeNoOptimization:
    return OptimizationMode::NoOptimization;
  }
  llvm_unreachable("unhandled enum value");
}

BridgedOptimizeAttr BridgedOptimizeAttr_createParsed(
    BridgedASTContext cContext, BridgedSourceLoc cAtLoc,
    BridgedSourceRange cRange, BridgedOptimizationMode cMode) {

  return new (cContext.unbridged())
      OptimizeAttr(cAtLoc.unbridged(), cRange.unbridged(), unbridged(cMode));
}

BridgedPrivateImportAttr BridgedPrivateImportAttr_createParsed(
    BridgedASTContext cContext, BridgedSourceLoc cAtLoc,
    BridgedSourceLoc cAttrNameLoc, BridgedSourceLoc cLParenLoc,
    BridgedStringRef cFileName, BridgedSourceLoc cRParenLoc) {
  return PrivateImportAttr::create(
      cContext.unbridged(), cAtLoc.unbridged(), cAttrNameLoc.unbridged(),
      cLParenLoc.unbridged(), cFileName.unbridged(), cRParenLoc.unbridged());
}

BridgedProjectedValuePropertyAttr
BridgedProjectedValuePropertyAttr_createParsed(BridgedASTContext cContext,
                                               BridgedSourceLoc cAtLoc,
                                               BridgedSourceRange cRange,
                                               BridgedIdentifier cName) {
  return new (cContext.unbridged())
      ProjectedValuePropertyAttr(cName.unbridged(), cAtLoc.unbridged(),
                                 cRange.unbridged(), /*Implicit=*/false);
}

BridgedRawDocCommentAttr
BridgedRawDocCommentAttr_createParsed(BridgedASTContext cContext,
                                      BridgedCharSourceRange cRange) {
  return new (cContext.unbridged()) RawDocCommentAttr(cRange.unbridged());
}

static ReferenceOwnership unbridged(BridgedReferenceOwnership kind) {
  switch (kind) {
  case BridgedReferenceOwnershipWeak:
    return ReferenceOwnership::Weak;
  case BridgedReferenceOwnershipUnowned:
    return ReferenceOwnership::Unowned;
  case BridgedReferenceOwnershipUnmanaged:
    return ReferenceOwnership::Unmanaged;
  }
  llvm_unreachable("unhandled enum value");
}

BridgedReferenceOwnershipAttr BridgedReferenceOwnershipAttr_createParsed(
    BridgedASTContext cContext, BridgedSourceLoc cAtLoc,
    BridgedSourceRange cRange, BridgedReferenceOwnership cKind) {
  return new (cContext.unbridged())
      ReferenceOwnershipAttr(cRange.unbridged(), unbridged(cKind));
}

BridgedSectionAttr BridgedSectionAttr_createParsed(BridgedASTContext cContext,
                                                   BridgedSourceLoc cAtLoc,
                                                   BridgedSourceRange cRange,
                                                   BridgedStringRef cName) {
  return new (cContext.unbridged())
      SectionAttr(cName.unbridged(), cAtLoc.unbridged(), cRange.unbridged(),
                  /*Implicit=*/false);
}

BridgedSemanticsAttr BridgedSemanticsAttr_createParsed(
    BridgedASTContext cContext, BridgedSourceLoc cAtLoc,
    BridgedSourceRange cRange, BridgedStringRef cValue) {
  return new (cContext.unbridged())
      SemanticsAttr(cValue.unbridged(), cAtLoc.unbridged(), cRange.unbridged(),
                    /*Implicit=*/false);
}

BridgedSetterAccessAttr
BridgedSetterAccessAttr_createParsed(BridgedASTContext cContext,
                                     BridgedSourceRange cRange,
                                     BridgedAccessLevel cAccessLevel) {
  return new (cContext.unbridged()) SetterAccessAttr(
      /*atLoc=*/{}, cRange.unbridged(), unbridged(cAccessLevel));
}

BridgedSPIAccessControlAttr BridgedSPIAccessControlAttr_createParsed(
    BridgedASTContext cContext, BridgedSourceLoc cAtLoc,
    BridgedSourceRange cRange, BridgedIdentifier cSPIGroupName) {

  return SPIAccessControlAttr::create(cContext.unbridged(), cAtLoc.unbridged(),
                                      cRange.unbridged(),
                                      cSPIGroupName.unbridged());
}

BridgedSILGenNameAttr BridgedSILGenNameAttr_createParsed(
    BridgedASTContext cContext, BridgedSourceLoc cAtLoc,
    BridgedSourceRange cRange, BridgedStringRef cName, bool isRaw) {
  return new (cContext.unbridged())
      SILGenNameAttr(cName.unbridged(), isRaw, cAtLoc.unbridged(),
                     cRange.unbridged(), /*Implicit=*/false);
}

//===----------------------------------------------------------------------===//
// MARK: Decls
//===----------------------------------------------------------------------===//

void BridgedDecl_setAttrs(BridgedDecl decl, BridgedDeclAttributes attrs) {
  decl.unbridged()->getAttrs() = attrs.unbridged();
}

BridgedAccessorDecl BridgedAccessorDecl_createParsed(
    BridgedASTContext cContext, BridgedDeclContext cDeclContext,
    BridgedAccessorKind cKind, BridgedAbstractStorageDecl cStorage,
    BridgedSourceLoc cDeclLoc, BridgedSourceLoc cAccessorKeywordLoc,
    BridgedNullableParameterList cParamList, BridgedSourceLoc cAsyncLoc,
    BridgedSourceLoc cThrowsLoc, BridgedNullableTypeRepr cThrownType) {
  return AccessorDecl::createParsed(
      cContext.unbridged(), unbridged(cKind), cStorage.unbridged(),
      cDeclLoc.unbridged(), cAccessorKeywordLoc.unbridged(),
      cParamList.unbridged(), cAsyncLoc.unbridged(), cThrowsLoc.unbridged(),
      cThrownType.unbridged(), cDeclContext.unbridged());
}

BridgedPatternBindingDecl BridgedPatternBindingDecl_createParsed(
    BridgedASTContext cContext, BridgedDeclContext cDeclContext,
    BridgedSourceLoc cBindingKeywordLoc, BridgedArrayRef cBindingEntries,
    bool isStatic, bool isLet) {
  ASTContext &context = cContext.unbridged();
  DeclContext *declContext = cDeclContext.unbridged();

  auto introducer = isLet ? VarDecl::Introducer::Let : VarDecl::Introducer::Var;

  SmallVector<PatternBindingEntry, 4> entries;
  for (auto &entry : cBindingEntries.unbridged<BridgedPatternBindingEntry>()) {
    auto *pattern = entry.pattern.unbridged();

    // Configure all vars.
    pattern->forEachVariable([&](VarDecl *VD) {
      VD->setStatic(isStatic);
      VD->setIntroducer(introducer);
    });

    entries.emplace_back(pattern, entry.equalLoc.unbridged(),
                         entry.init.unbridged(), entry.initContext.unbridged());
  }

  return PatternBindingDecl::create(
      context,
      /*StaticLoc=*/SourceLoc(),
      // FIXME: 'class' spelling kind.
      isStatic ? StaticSpellingKind::KeywordStatic : StaticSpellingKind::None,
      cBindingKeywordLoc.unbridged(), entries, declContext);
}

BridgedParamDecl BridgedParamDecl_createParsed(
    BridgedASTContext cContext, BridgedDeclContext cDeclContext,
    BridgedSourceLoc cSpecifierLoc, BridgedIdentifier cArgName,
    BridgedSourceLoc cArgNameLoc, BridgedIdentifier cParamName,
    BridgedSourceLoc cParamNameLoc, BridgedNullableTypeRepr opaqueType,
    BridgedNullableExpr opaqueDefaultValue) {
  auto *paramDecl = ParamDecl::createParsed(
      cContext.unbridged(), cSpecifierLoc.unbridged(), cArgNameLoc.unbridged(),
      cArgName.unbridged(), cParamNameLoc.unbridged(), cParamName.unbridged(),
      opaqueDefaultValue.unbridged(), cDeclContext.unbridged());

  if (auto type = opaqueType.unbridged()) {
    paramDecl->setTypeRepr(type);

    // FIXME: Copied from 'Parser::parsePattern()'. This should be in Sema.
    // Dig through the type to find any attributes or modifiers that are
    // associated with the type but should also be reflected on the
    // declaration.
    auto unwrappedType = type;
    while (true) {
      if (auto *ATR = dyn_cast<AttributedTypeRepr>(unwrappedType)) {
        auto attrs = ATR->getAttrs();
        // At this point we actually don't know if that's valid to mark
        // this parameter declaration as `autoclosure` because type has
        // not been resolved yet - it should either be a function type
        // or typealias with underlying function type.
        bool autoclosure = llvm::any_of(attrs, [](TypeOrCustomAttr attr) {
          if (auto typeAttr = attr.dyn_cast<TypeAttribute*>())
            return isa<AutoclosureTypeAttr>(typeAttr);
          return false;
        });
        paramDecl->setAutoClosure(autoclosure);

        unwrappedType = ATR->getTypeRepr();
        continue;
      }

      if (auto *STR = dyn_cast<SpecifierTypeRepr>(unwrappedType)) {
        if (isa<IsolatedTypeRepr>(STR))
          paramDecl->setIsolated(true);
        else if (isa<CompileTimeConstTypeRepr>(STR))
          paramDecl->setCompileTimeConst(true);
        else if (isa<TransferringTypeRepr>(STR))
          paramDecl->setSending(true);
        else if (isa<SendingTypeRepr>(STR))
          paramDecl->setSending(true);

        unwrappedType = STR->getBase();
        continue;
      }

      break;
    }
  }

  return paramDecl;
}

void BridgedConstructorDecl_setParsedBody(BridgedConstructorDecl decl,
                                          BridgedBraceStmt body) {
  decl.unbridged()->setBody(body.unbridged(), FuncDecl::BodyKind::Parsed);
}

void BridgedFuncDecl_setParsedBody(BridgedFuncDecl decl,
                                   BridgedBraceStmt body) {
  decl.unbridged()->setBody(body.unbridged(), FuncDecl::BodyKind::Parsed);
}

void BridgedDestructorDecl_setParsedBody(BridgedDestructorDecl decl,
                                         BridgedBraceStmt body) {
  decl.unbridged()->setBody(body.unbridged(), FuncDecl::BodyKind::Parsed);
}

BridgedFuncDecl BridgedFuncDecl_createParsed(
    BridgedASTContext cContext, BridgedDeclContext cDeclContext,
    BridgedSourceLoc cStaticLoc, BridgedStaticSpelling cStaticSpelling,
    BridgedSourceLoc cFuncKeywordLoc, BridgedIdentifier cName,
    BridgedSourceLoc cNameLoc, BridgedNullableGenericParamList genericParamList,
    BridgedParameterList parameterList, BridgedSourceLoc cAsyncLoc,
    BridgedSourceLoc cThrowsLoc, BridgedNullableTypeRepr thrownType,
    BridgedNullableTypeRepr returnType,
    BridgedNullableTrailingWhereClause genericWhereClause) {
  ASTContext &context = cContext.unbridged();

  auto *paramList = parameterList.unbridged();
  auto declName = DeclName(context, cName.unbridged(), paramList);
  auto asyncLoc = cAsyncLoc.unbridged();
  auto throwsLoc = cThrowsLoc.unbridged();
  // FIXME: rethrows

  auto *decl = FuncDecl::create(
      context, cStaticLoc.unbridged(), unbridged(cStaticSpelling),
      cFuncKeywordLoc.unbridged(), declName, cNameLoc.unbridged(),
      asyncLoc.isValid(), asyncLoc, throwsLoc.isValid(), throwsLoc,
      thrownType.unbridged(), genericParamList.unbridged(), paramList,
      returnType.unbridged(), cDeclContext.unbridged());
  decl->setTrailingWhereClause(genericWhereClause.unbridged());

  return decl;
}

BridgedConstructorDecl BridgedConstructorDecl_createParsed(
    BridgedASTContext cContext, BridgedDeclContext cDeclContext,
    BridgedSourceLoc cInitKeywordLoc, BridgedSourceLoc cFailabilityMarkLoc,
    bool isIUO, BridgedNullableGenericParamList genericParams,
    BridgedParameterList bridgedParameterList, BridgedSourceLoc cAsyncLoc,
    BridgedSourceLoc cThrowsLoc, BridgedNullableTypeRepr thrownType,
    BridgedNullableTrailingWhereClause genericWhereClause) {
  assert(cFailabilityMarkLoc.unbridged().isValid() || !isIUO);

  ASTContext &context = cContext.unbridged();

  auto *parameterList = bridgedParameterList.unbridged();
  auto declName =
      DeclName(context, DeclBaseName::createConstructor(), parameterList);
  auto asyncLoc = cAsyncLoc.unbridged();
  auto throwsLoc = cThrowsLoc.unbridged();
  auto failabilityMarkLoc = cFailabilityMarkLoc.unbridged();
  // FIXME: rethrows
  // TODO: Handle LifetimeDependentReturnTypeRepr here.
  auto *decl = new (context) ConstructorDecl(
      declName, cInitKeywordLoc.unbridged(), failabilityMarkLoc.isValid(),
      failabilityMarkLoc, asyncLoc.isValid(), asyncLoc, throwsLoc.isValid(),
      throwsLoc, thrownType.unbridged(), parameterList,
      genericParams.unbridged(), cDeclContext.unbridged(),
      /*InitRetTy*/ nullptr);
  decl->setTrailingWhereClause(genericWhereClause.unbridged());
  decl->setImplicitlyUnwrappedOptional(isIUO);

  return decl;
}

BridgedDestructorDecl
BridgedDestructorDecl_createParsed(BridgedASTContext cContext,
                                   BridgedDeclContext cDeclContext,
                                   BridgedSourceLoc cDeinitKeywordLoc) {
  ASTContext &context = cContext.unbridged();
  auto *decl = new (context)
      DestructorDecl(cDeinitKeywordLoc.unbridged(), cDeclContext.unbridged());

  return decl;
}

BridgedTypeAliasDecl BridgedTypeAliasDecl_createParsed(
    BridgedASTContext cContext, BridgedDeclContext cDeclContext,
    BridgedSourceLoc cAliasKeywordLoc, BridgedIdentifier cName,
    BridgedSourceLoc cNameLoc, BridgedNullableGenericParamList genericParamList,
    BridgedSourceLoc cEqualLoc, BridgedTypeRepr opaqueUnderlyingType,
    BridgedNullableTrailingWhereClause genericWhereClause) {
  ASTContext &context = cContext.unbridged();

  auto *decl = new (context)
      TypeAliasDecl(cAliasKeywordLoc.unbridged(), cEqualLoc.unbridged(),
                    cName.unbridged(), cNameLoc.unbridged(),
                    genericParamList.unbridged(), cDeclContext.unbridged());
  decl->setUnderlyingTypeRepr(opaqueUnderlyingType.unbridged());
  decl->setTrailingWhereClause(genericWhereClause.unbridged());

  return decl;
}

static void setParsedMembers(IterableDeclContext *IDC,
                             BridgedArrayRef bridgedMembers) {
  auto &ctx = IDC->getDecl()->getASTContext();

  SmallVector<Decl *> members;
  for (auto *decl : bridgedMembers.unbridged<Decl *>()) {
    members.push_back(decl);

    // Add any variables bound to the list of decls.
    if (auto *PBD = dyn_cast<PatternBindingDecl>(decl)) {
      for (auto idx : range(PBD->getNumPatternEntries())) {
        PBD->getPattern(idx)->forEachVariable([&](VarDecl *VD) {
          members.push_back(VD);
        });
      }
    }
    // Each enum case element is also part of the members list according to the
    // legacy parser.
    if (auto *ECD = dyn_cast<EnumCaseDecl>(decl)) {
      for (auto *EED : ECD->getElements()) {
        members.push_back(EED);
      }
    }
  }

  ctx.evaluator.cacheOutput(
      ParseMembersRequest{IDC},
      FingerprintAndMembers{std::nullopt, ctx.AllocateCopy(members)});
}

void BridgedNominalTypeDecl_setParsedMembers(BridgedNominalTypeDecl bridgedDecl,
                                             BridgedArrayRef bridgedMembers) {
  setParsedMembers(bridgedDecl.unbridged(), bridgedMembers);
}

void BridgedExtensionDecl_setParsedMembers(BridgedExtensionDecl bridgedDecl,
                                           BridgedArrayRef bridgedMembers) {
  setParsedMembers(bridgedDecl.unbridged(), bridgedMembers);
}

static ArrayRef<InheritedEntry>
convertToInheritedEntries(ASTContext &ctx, BridgedArrayRef cInheritedTypes) {
  return ctx.AllocateTransform<InheritedEntry>(
      cInheritedTypes.unbridged<BridgedTypeRepr>(),
      [](auto &e) { return InheritedEntry(e.unbridged()); });
}

BridgedNominalTypeDecl BridgedEnumDecl_createParsed(
    BridgedASTContext cContext, BridgedDeclContext cDeclContext,
    BridgedSourceLoc cEnumKeywordLoc, BridgedIdentifier cName,
    BridgedSourceLoc cNameLoc, BridgedNullableGenericParamList genericParamList,
    BridgedArrayRef cInheritedTypes,
    BridgedNullableTrailingWhereClause genericWhereClause,
    BridgedSourceRange cBraceRange) {
  ASTContext &context = cContext.unbridged();

  NominalTypeDecl *decl = new (context) EnumDecl(
      cEnumKeywordLoc.unbridged(), cName.unbridged(), cNameLoc.unbridged(),
      convertToInheritedEntries(context, cInheritedTypes),
      genericParamList.unbridged(), cDeclContext.unbridged());
  decl->setTrailingWhereClause(genericWhereClause.unbridged());
  decl->setBraces(cBraceRange.unbridged());

  return decl;
}

BridgedEnumCaseDecl
BridgedEnumCaseDecl_createParsed(BridgedDeclContext cDeclContext,
                                 BridgedSourceLoc cCaseKeywordLoc,
                                 BridgedArrayRef cElements) {
  return EnumCaseDecl::create(cCaseKeywordLoc.unbridged(),
                              cElements.unbridged<EnumElementDecl *>(),
                              cDeclContext.unbridged());
}

BridgedEnumElementDecl BridgedEnumElementDecl_createParsed(
    BridgedASTContext cContext, BridgedDeclContext cDeclContext,
    BridgedIdentifier cName, BridgedSourceLoc cNameLoc,
    BridgedNullableParameterList bridgedParameterList,
    BridgedSourceLoc cEqualsLoc, BridgedNullableExpr rawValue) {
  ASTContext &context = cContext.unbridged();

  auto *parameterList = bridgedParameterList.unbridged();
  DeclName declName;
  {
    auto identifier = cName.unbridged();
    if (parameterList) {
      declName = DeclName(context, identifier, parameterList);
    } else {
      declName = identifier;
    }
  }

  return new (context) EnumElementDecl(
      cNameLoc.unbridged(), declName, parameterList, cEqualsLoc.unbridged(),
      cast_or_null<LiteralExpr>(rawValue.unbridged()),
      cDeclContext.unbridged());
}

BridgedNominalTypeDecl BridgedStructDecl_createParsed(
    BridgedASTContext cContext, BridgedDeclContext cDeclContext,
    BridgedSourceLoc cStructKeywordLoc, BridgedIdentifier cName,
    BridgedSourceLoc cNameLoc, BridgedNullableGenericParamList genericParamList,
    BridgedArrayRef cInheritedTypes,
    BridgedNullableTrailingWhereClause genericWhereClause,
    BridgedSourceRange cBraceRange) {
  ASTContext &context = cContext.unbridged();

  NominalTypeDecl *decl = new (context) StructDecl(
      cStructKeywordLoc.unbridged(), cName.unbridged(), cNameLoc.unbridged(),
      convertToInheritedEntries(context, cInheritedTypes),
      genericParamList.unbridged(), cDeclContext.unbridged());
  decl->setTrailingWhereClause(genericWhereClause.unbridged());
  decl->setBraces(cBraceRange.unbridged());

  return decl;
}

BridgedNominalTypeDecl BridgedClassDecl_createParsed(
    BridgedASTContext cContext, BridgedDeclContext cDeclContext,
    BridgedSourceLoc cClassKeywordLoc, BridgedIdentifier cName,
    BridgedSourceLoc cNameLoc, BridgedNullableGenericParamList genericParamList,
    BridgedArrayRef cInheritedTypes,
    BridgedNullableTrailingWhereClause genericWhereClause,
    BridgedSourceRange cBraceRange, bool isActor) {
  ASTContext &context = cContext.unbridged();

  NominalTypeDecl *decl = new (context) ClassDecl(
      cClassKeywordLoc.unbridged(), cName.unbridged(), cNameLoc.unbridged(),
      convertToInheritedEntries(context, cInheritedTypes),
      genericParamList.unbridged(), cDeclContext.unbridged(), isActor);
  decl->setTrailingWhereClause(genericWhereClause.unbridged());
  decl->setBraces(cBraceRange.unbridged());

  return decl;
}

BridgedNominalTypeDecl BridgedProtocolDecl_createParsed(
    BridgedASTContext cContext, BridgedDeclContext cDeclContext,
    BridgedSourceLoc cProtocolKeywordLoc, BridgedIdentifier cName,
    BridgedSourceLoc cNameLoc, BridgedArrayRef cPrimaryAssociatedTypeNames,
    BridgedArrayRef cInheritedTypes,
    BridgedNullableTrailingWhereClause genericWhereClause,
    BridgedSourceRange cBraceRange) {
  ASTContext &context = cContext.unbridged();

  auto primaryAssociatedTypeNames =
      context.AllocateTransform<PrimaryAssociatedTypeName>(
          cPrimaryAssociatedTypeNames.unbridged<BridgedLocatedIdentifier>(),
          [](auto &e) -> PrimaryAssociatedTypeName {
            return {e.Name.unbridged(), e.NameLoc.unbridged()};
          });

  NominalTypeDecl *decl = new (context) ProtocolDecl(
      cDeclContext.unbridged(), cProtocolKeywordLoc.unbridged(),
      cNameLoc.unbridged(), cName.unbridged(), primaryAssociatedTypeNames,
      convertToInheritedEntries(context, cInheritedTypes),
      genericWhereClause.unbridged());
  decl->setBraces(cBraceRange.unbridged());

  return decl;
}

BridgedAssociatedTypeDecl BridgedAssociatedTypeDecl_createParsed(
    BridgedASTContext cContext, BridgedDeclContext cDeclContext,
    BridgedSourceLoc cAssociatedtypeKeywordLoc, BridgedIdentifier cName,
    BridgedSourceLoc cNameLoc, BridgedArrayRef cInheritedTypes,
    BridgedNullableTypeRepr defaultType,
    BridgedNullableTrailingWhereClause genericWhereClause) {
  ASTContext &context = cContext.unbridged();

  auto *decl = AssociatedTypeDecl::createParsed(
      context, cDeclContext.unbridged(), cAssociatedtypeKeywordLoc.unbridged(),
      cName.unbridged(), cNameLoc.unbridged(), defaultType.unbridged(),
      genericWhereClause.unbridged());
  decl->setInherited(convertToInheritedEntries(context, cInheritedTypes));

  return decl;
}

BridgedExtensionDecl BridgedExtensionDecl_createParsed(
    BridgedASTContext cContext, BridgedDeclContext cDeclContext,
    BridgedSourceLoc cExtensionKeywordLoc, BridgedTypeRepr extendedType,
    BridgedArrayRef cInheritedTypes,
    BridgedNullableTrailingWhereClause genericWhereClause,
    BridgedSourceRange cBraceRange) {
  ASTContext &context = cContext.unbridged();

  auto *decl = ExtensionDecl::create(
      context, cExtensionKeywordLoc.unbridged(), extendedType.unbridged(),
      convertToInheritedEntries(context, cInheritedTypes),
      cDeclContext.unbridged(), genericWhereClause.unbridged());
  decl->setBraces(cBraceRange.unbridged());
  return decl;
}

BridgedOperatorDecl BridgedOperatorDecl_createParsed(
    BridgedASTContext cContext, BridgedDeclContext cDeclContext,
    BridgedOperatorFixity cFixity, BridgedSourceLoc cOperatorKeywordLoc,
    BridgedIdentifier cName, BridgedSourceLoc cNameLoc,
    BridgedSourceLoc cColonLoc, BridgedIdentifier cPrecedenceGroupName,
    BridgedSourceLoc cPrecedenceGroupLoc) {
  auto colonLoc = cColonLoc.unbridged();
  assert(colonLoc.isValid() == cPrecedenceGroupName.unbridged().nonempty());
  assert(colonLoc.isValid() == cPrecedenceGroupLoc.unbridged().isValid());

  ASTContext &context = cContext.unbridged();
  auto operatorKeywordLoc = cOperatorKeywordLoc.unbridged();
  auto name = cName.unbridged();
  auto nameLoc = cNameLoc.unbridged();
  auto *declContext = cDeclContext.unbridged();

  OperatorDecl *decl = nullptr;
  switch (cFixity) {
  case BridgedOperatorFixityInfix:
    decl = new (context) InfixOperatorDecl(
        declContext, operatorKeywordLoc, name, nameLoc, cColonLoc.unbridged(),
        cPrecedenceGroupName.unbridged(), cPrecedenceGroupLoc.unbridged());
    break;
  case BridgedOperatorFixityPrefix:
    assert(colonLoc.isInvalid());
    decl = new (context)
        PrefixOperatorDecl(declContext, operatorKeywordLoc, name, nameLoc);
    break;
  case BridgedOperatorFixityPostfix:
    assert(colonLoc.isInvalid());
    decl = new (context)
        PostfixOperatorDecl(declContext, operatorKeywordLoc, name, nameLoc);
    break;
  }

  return decl;
}

BridgedPrecedenceGroupDecl BridgedPrecedenceGroupDecl_createParsed(
    BridgedDeclContext cDeclContext,
    BridgedSourceLoc cPrecedencegroupKeywordLoc, BridgedIdentifier cName,
    BridgedSourceLoc cNameLoc, BridgedSourceLoc cLeftBraceLoc,
    BridgedSourceLoc cAssociativityKeywordLoc,
    BridgedSourceLoc cAssociativityValueLoc,
    BridgedAssociativity cAssociativity, BridgedSourceLoc cAssignmentKeywordLoc,
    BridgedSourceLoc cAssignmentValueLoc, bool isAssignment,
    BridgedSourceLoc cHigherThanKeywordLoc, BridgedArrayRef cHigherThanNames,
    BridgedSourceLoc cLowerThanKeywordLoc, BridgedArrayRef cLowerThanNames,
    BridgedSourceLoc cRightBraceLoc) {

  SmallVector<PrecedenceGroupDecl::Relation, 2> higherThanNames;
  for (auto &pair : cHigherThanNames.unbridged<BridgedLocatedIdentifier>()) {
    higherThanNames.push_back(
        {pair.NameLoc.unbridged(), pair.Name.unbridged(), nullptr});
  }

  SmallVector<PrecedenceGroupDecl::Relation, 2> lowerThanNames;
  for (auto &pair : cLowerThanNames.unbridged<BridgedLocatedIdentifier>()) {
    lowerThanNames.push_back(
        {pair.NameLoc.unbridged(), pair.Name.unbridged(), nullptr});
  }

  return PrecedenceGroupDecl::create(
      cDeclContext.unbridged(), cPrecedencegroupKeywordLoc.unbridged(),
      cNameLoc.unbridged(), cName.unbridged(), cLeftBraceLoc.unbridged(),
      cAssociativityKeywordLoc.unbridged(), cAssociativityValueLoc.unbridged(),
      static_cast<Associativity>(cAssociativity),
      cAssignmentKeywordLoc.unbridged(), cAssignmentValueLoc.unbridged(),
      isAssignment, cHigherThanKeywordLoc.unbridged(), higherThanNames,
      cLowerThanKeywordLoc.unbridged(), lowerThanNames,
      cRightBraceLoc.unbridged());
}

BridgedImportDecl BridgedImportDecl_createParsed(
    BridgedASTContext cContext, BridgedDeclContext cDeclContext,
    BridgedSourceLoc cImportKeywordLoc, BridgedImportKind cImportKind,
    BridgedSourceLoc cImportKindLoc, BridgedArrayRef cImportPathElements) {
  ImportPath::Builder builder;
  for (auto &element :
       cImportPathElements.unbridged<BridgedLocatedIdentifier>()) {
    builder.push_back(element.Name.unbridged(), element.NameLoc.unbridged());
  }

  ASTContext &context = cContext.unbridged();
  return ImportDecl::create(
      context, cDeclContext.unbridged(), cImportKeywordLoc.unbridged(),
      static_cast<ImportKind>(cImportKind), cImportKindLoc.unbridged(),
      std::move(builder).get());
}

BridgedSubscriptDecl BridgedSubscriptDecl_createParsed(
    BridgedASTContext cContext, BridgedDeclContext cDeclContext,
    BridgedSourceLoc cStaticLoc, BridgedStaticSpelling cStaticSpelling,
    BridgedSourceLoc cSubscriptKeywordLoc,
    BridgedNullableGenericParamList cGenericParamList,
    BridgedParameterList cParamList, BridgedSourceLoc cArrowLoc,
    BridgedTypeRepr returnType) {
  return SubscriptDecl::createParsed(
      cContext.unbridged(), cStaticLoc.unbridged(), unbridged(cStaticSpelling),
      cSubscriptKeywordLoc.unbridged(), cParamList.unbridged(),
      cArrowLoc.unbridged(), returnType.unbridged(), cDeclContext.unbridged(),
      cGenericParamList.unbridged());
}

BridgedTopLevelCodeDecl BridgedTopLevelCodeDecl_createStmt(
    BridgedASTContext cContext, BridgedDeclContext cDeclContext,
    BridgedSourceLoc cStartLoc, BridgedStmt statement,
    BridgedSourceLoc cEndLoc) {
  ASTContext &context = cContext.unbridged();
  DeclContext *declContext = cDeclContext.unbridged();

  auto *S = statement.unbridged();
  auto Brace = BraceStmt::create(context, cStartLoc.unbridged(), {S},
                                 cEndLoc.unbridged(),
                                 /*Implicit=*/true);
  return new (context) TopLevelCodeDecl(declContext, Brace);
}

BridgedTopLevelCodeDecl BridgedTopLevelCodeDecl_createExpr(
    BridgedASTContext cContext, BridgedDeclContext cDeclContext,
    BridgedSourceLoc cStartLoc, BridgedExpr expression,
    BridgedSourceLoc cEndLoc) {
  ASTContext &context = cContext.unbridged();
  DeclContext *declContext = cDeclContext.unbridged();

  auto *E = expression.unbridged();
  auto Brace = BraceStmt::create(context, cStartLoc.unbridged(), {E},
                                 cEndLoc.unbridged(),
                                 /*Implicit=*/true);
  return new (context) TopLevelCodeDecl(declContext, Brace);
}

BridgedVarDecl BridgedVarDec_createImplicitStringInterpolationVar(
    BridgedDeclContext cDeclContext) {
  return VarDecl::createImplicitStringInterpolationVar(
      cDeclContext.unbridged());
}

//===----------------------------------------------------------------------===//
// MARK: AbstractStorageDecl
//===----------------------------------------------------------------------===//

void BridgedAbstractStorageDecl_setAccessors(
    BridgedAbstractStorageDecl cStorage, BridgedAccessorRecord accessors) {
  cStorage.unbridged()->setAccessors(
      accessors.lBraceLoc.unbridged(),
      accessors.accessors.unbridged<AccessorDecl *>(),
      accessors.rBraceLoc.unbridged());
}

//===----------------------------------------------------------------------===//
// MARK: AccessorDecl
//===----------------------------------------------------------------------===//

void BridgedAccessorDecl_setParsedBody(BridgedAccessorDecl decl,
                                       BridgedBraceStmt body) {
  decl.unbridged()->setBody(body.unbridged(),
                            AbstractFunctionDecl::BodyKind::Parsed);
}

//===----------------------------------------------------------------------===//
// MARK: BridgedNominalTypeDecl
//===----------------------------------------------------------------------===//

bool BridgedNominalTypeDecl_isStructWithUnreferenceableStorage(
    BridgedNominalTypeDecl decl) {
  if (auto *structDecl = dyn_cast<swift::StructDecl>(decl.unbridged())) {
    return structDecl->hasUnreferenceableStorage();
  }
  return false;
}

//===----------------------------------------------------------------------===//
// MARK: Exprs
//===----------------------------------------------------------------------===//

BridgedArgumentList
BridgedArgumentList_createImplicitUnlabeled(BridgedASTContext cContext,
                                            BridgedArrayRef cExprs) {
  return ArgumentList::forImplicitUnlabeled(cContext.unbridged(),
                                            cExprs.unbridged<Expr *>());
}

BridgedArgumentList BridgedArgumentList_createParsed(
    BridgedASTContext cContext, BridgedSourceLoc cLParenLoc,
    BridgedArrayRef cArgs, BridgedSourceLoc cRParenLoc,
    size_t cFirstTrailingClosureIndex) {
  SmallVector<Argument> arguments;
  arguments.reserve(cArgs.unbridged<BridgedCallArgument>().size());
  for (auto &arg : cArgs.unbridged<BridgedCallArgument>()) {
    arguments.push_back(arg.unbridged());
  }

  std::optional<unsigned int> firstTrailingClosureIndex;
  if (cFirstTrailingClosureIndex < arguments.size())
    firstTrailingClosureIndex = cFirstTrailingClosureIndex;

  return ArgumentList::createParsed(
      cContext.unbridged(), cLParenLoc.unbridged(), arguments,
      cRParenLoc.unbridged(), firstTrailingClosureIndex);
}

BridgedArrayExpr BridgedArrayExpr_createParsed(BridgedASTContext cContext,
                                               BridgedSourceLoc cLLoc,
                                               BridgedArrayRef elements,
                                               BridgedArrayRef commas,
                                               BridgedSourceLoc cRLoc) {
  ASTContext &context = cContext.unbridged();
  return ArrayExpr::create(context, cLLoc.unbridged(),
                           elements.unbridged<Expr *>(),
                           commas.unbridged<SourceLoc>(), cRLoc.unbridged());
}

BridgedArrowExpr BridgedArrowExpr_createParsed(BridgedASTContext cContext,
                                               BridgedSourceLoc cAsyncLoc,
                                               BridgedSourceLoc cThrowsLoc,
                                               BridgedNullableExpr cThrownType,
                                               BridgedSourceLoc cArrowLoc) {
  return new (cContext.unbridged())
      ArrowExpr(cAsyncLoc.unbridged(), cThrowsLoc.unbridged(),
                cThrownType.unbridged(), cArrowLoc.unbridged());
}

BridgedAssignExpr BridgedAssignExpr_createParsed(BridgedASTContext cContext,
                                                 BridgedSourceLoc cEqualsLoc) {
  return new (cContext.unbridged()) AssignExpr(cEqualsLoc.unbridged());
}

BridgedAwaitExpr BridgedAwaitExpr_createParsed(BridgedASTContext cContext,
                                               BridgedSourceLoc cAwaitLoc,
                                               BridgedExpr cSubExpr) {
  return new (cContext.unbridged())
      AwaitExpr(cAwaitLoc.unbridged(), cSubExpr.unbridged());
}

BridgedBooleanLiteralExpr
BridgedBooleanLiteralExpr_createParsed(BridgedASTContext cContext, bool value,
                                       BridgedSourceLoc cTokenLoc) {
  ASTContext &context = cContext.unbridged();
  return new (context) BooleanLiteralExpr(value, cTokenLoc.unbridged());
}

BridgedBorrowExpr BridgedBorrowExpr_createParsed(BridgedASTContext cContext,
                                                 BridgedSourceLoc cBorrowLoc,
                                                 BridgedExpr cSubExpr) {
  return new (cContext.unbridged())
      BorrowExpr(cBorrowLoc.unbridged(), cSubExpr.unbridged());
}

BridgedCallExpr BridgedCallExpr_createParsed(BridgedASTContext cContext,
                                             BridgedExpr fn,
                                             BridgedArgumentList cArguments) {
  return CallExpr::create(cContext.unbridged(), fn.unbridged(),
                          cArguments.unbridged(),
                          /*implicit*/ false);
}

BridgedClosureExpr BridgedClosureExpr_createParsed(
    BridgedASTContext cContext, BridgedDeclContext cDeclContext,
    BridgedParameterList cParamList, BridgedBraceStmt body) {
  DeclAttributes attributes;
  SourceRange bracketRange;
  SourceLoc asyncLoc;
  SourceLoc throwsLoc;
  SourceLoc arrowLoc;
  SourceLoc inLoc;

  ASTContext &context = cContext.unbridged();
  DeclContext *declContext = cDeclContext.unbridged();

  auto *out = new (context) ClosureExpr(
      attributes, bracketRange, nullptr, cParamList.unbridged(), asyncLoc,
      throwsLoc,
      /*FIXME:thrownType=*/nullptr, arrowLoc, inLoc, nullptr, declContext);
  out->setBody(body.unbridged());
  return out;
}

BridgedCoerceExpr BridgedCoerceExpr_createParsed(BridgedASTContext cContext,
                                                 BridgedSourceLoc cAsLoc,
                                                 BridgedTypeRepr cType) {
  return CoerceExpr::create(cContext.unbridged(), cAsLoc.unbridged(),
                            cType.unbridged());
}

BridgedConditionalCheckedCastExpr
BridgedConditionalCheckedCastExpr_createParsed(BridgedASTContext cContext,
                                               BridgedSourceLoc cAsLoc,
                                               BridgedSourceLoc cQuestionLoc,
                                               BridgedTypeRepr cType) {
  return ConditionalCheckedCastExpr::create(
      cContext.unbridged(), cAsLoc.unbridged(), cQuestionLoc.unbridged(),
      cType.unbridged());
}

BridgedConsumeExpr BridgedConsumeExpr_createParsed(BridgedASTContext cContext,
                                                   BridgedSourceLoc cConsumeLoc,
                                                   BridgedExpr cSubExpr) {
  return new (cContext.unbridged())
      ConsumeExpr(cConsumeLoc.unbridged(), cSubExpr.unbridged());
}

BridgedCopyExpr BridgedCopyExpr_createParsed(BridgedASTContext cContext,
                                             BridgedSourceLoc cCopyLoc,
                                             BridgedExpr cSubExpr) {
  return new (cContext.unbridged())
      CopyExpr(cCopyLoc.unbridged(), cSubExpr.unbridged());
}

BridgedDeclRefExpr BridgedDeclRefExpr_create(BridgedASTContext cContext,
                                             BridgedDecl cDecl,
                                             BridgedDeclNameLoc cLoc,
                                             bool IsImplicit) {
  return new (cContext.unbridged()) DeclRefExpr(
      cast<ValueDecl>(cDecl.unbridged()), cLoc.unbridged(), IsImplicit);
}

BridgedDictionaryExpr BridgedDictionaryExpr_createParsed(
    BridgedASTContext cContext, BridgedSourceLoc cLBracketLoc,
    BridgedArrayRef cElements, BridgedArrayRef cCommaLocs,
    BridgedSourceLoc cRBracketLoc) {
  return DictionaryExpr::create(cContext.unbridged(), cLBracketLoc.unbridged(),
                                cElements.unbridged<Expr *>(),
                                cCommaLocs.unbridged<SourceLoc>(),
                                cRBracketLoc.unbridged());
}

BridgedDiscardAssignmentExpr
BridgedDiscardAssignmentExpr_createParsed(BridgedASTContext cContext,
                                          BridgedSourceLoc cLoc) {
  return new (cContext.unbridged())
      DiscardAssignmentExpr(cLoc.unbridged(), /*Implicit=*/false);
}

BridgedDotSelfExpr BridgedDotSelfExpr_createParsed(BridgedASTContext cContext,
                                                   BridgedExpr cSubExpr,
                                                   BridgedSourceLoc cDotLoc,
                                                   BridgedSourceLoc cSelfLoc) {

  return new (cContext.unbridged()) DotSelfExpr(
      cSubExpr.unbridged(), cDotLoc.unbridged(), cSelfLoc.unbridged());
}

BridgedForceTryExpr
BridgedForceTryExpr_createParsed(BridgedASTContext cContext,
                                 BridgedSourceLoc cTryLoc, BridgedExpr cSubExpr,
                                 BridgedSourceLoc cExclaimLoc) {
  return new (cContext.unbridged()) ForceTryExpr(
      cTryLoc.unbridged(), cSubExpr.unbridged(), cExclaimLoc.unbridged());
}

BridgedForcedCheckedCastExpr BridgedForcedCheckedCastExpr_createParsed(
    BridgedASTContext cContext, BridgedSourceLoc cAsLoc,
    BridgedSourceLoc cExclaimLoc, BridgedTypeRepr cType) {
  return ForcedCheckedCastExpr::create(cContext.unbridged(), cAsLoc.unbridged(),
                                       cExclaimLoc.unbridged(),
                                       cType.unbridged());
}

BridgedIntegerLiteralExpr
BridgedIntegerLiteralExpr_createParsed(BridgedASTContext cContext,
                                       BridgedStringRef cStr,
                                       BridgedSourceLoc cTokenLoc) {
  ASTContext &context = cContext.unbridged();
  auto str = context.AllocateCopy(cStr.unbridged());
  return new (context) IntegerLiteralExpr(str, cTokenLoc.unbridged());
}

BridgedInterpolatedStringLiteralExpr
BridgedInterpolatedStringLiteralExpr_createParsed(
    BridgedASTContext cContext, BridgedSourceLoc cLoc, size_t literalCapacity,
    size_t interpolationCount, BridgedTapExpr cAppendingExpr) {
  return new (cContext.unbridged()) InterpolatedStringLiteralExpr(
      cLoc.unbridged(), literalCapacity, interpolationCount,
      cAppendingExpr.unbridged());
}

BridgedIsExpr BridgedIsExpr_createParsed(BridgedASTContext cContext,
                                         BridgedSourceLoc cIsLoc,
                                         BridgedTypeRepr cType) {
  return IsExpr::create(cContext.unbridged(), cIsLoc.unbridged(),
                        cType.unbridged());
}

BridgedNilLiteralExpr
BridgedNilLiteralExpr_createParsed(BridgedASTContext cContext,
                                   BridgedSourceLoc cNilKeywordLoc) {
  return new (cContext.unbridged()) NilLiteralExpr(cNilKeywordLoc.unbridged());
}

BridgedOptionalTryExpr BridgedOptionalTryExpr_createParsed(
    BridgedASTContext cContext, BridgedSourceLoc cTryLoc, BridgedExpr cSubExpr,
    BridgedSourceLoc cQuestionLoc) {
  return new (cContext.unbridged()) OptionalTryExpr(
      cTryLoc.unbridged(), cSubExpr.unbridged(), cQuestionLoc.unbridged());
}

BridgedPackElementExpr
BridgedPackElementExpr_createParsed(BridgedASTContext cContext,
                                    BridgedSourceLoc cEachLoc,
                                    BridgedExpr cPackRefExpr) {
  return PackElementExpr::create(cContext.unbridged(), cEachLoc.unbridged(),
                                 cPackRefExpr.unbridged());
}

BridgedPackExpansionExpr
BridgedPackExpansionExpr_createParsed(BridgedASTContext cContext,
                                      BridgedSourceLoc cRepeatLoc,
                                      BridgedExpr cPatternExpr) {
  return PackExpansionExpr::create(cContext.unbridged(), cRepeatLoc.unbridged(),
                                   cPatternExpr.unbridged(),
                                   /*genericEnv=*/nullptr);
}

BridgedPostfixUnaryExpr
BridgedPostfixUnaryExpr_createParsed(BridgedASTContext cContext,
                                     BridgedExpr oper, BridgedExpr operand) {
  return PostfixUnaryExpr::create(cContext.unbridged(), oper.unbridged(),
                                  operand.unbridged());
}

BridgedPrefixUnaryExpr
BridgedPrefixUnaryExpr_createParsed(BridgedASTContext cContext,
                                    BridgedExpr oper, BridgedExpr operand) {
  return PrefixUnaryExpr::create(cContext.unbridged(), oper.unbridged(),
                                 operand.unbridged());
}

BridgedSequenceExpr BridgedSequenceExpr_createParsed(BridgedASTContext cContext,
                                                     BridgedArrayRef exprs) {
  return SequenceExpr::create(cContext.unbridged(), exprs.unbridged<Expr *>());
}

BridgedSingleValueStmtExpr BridgedSingleValueStmtExpr_createWithWrappedBranches(
    BridgedASTContext cContext, BridgedStmt S, BridgedDeclContext cDeclContext,
    bool mustBeExpr) {
  ASTContext &context = cContext.unbridged();
  DeclContext *declContext = cDeclContext.unbridged();
  return SingleValueStmtExpr::createWithWrappedBranches(
      context, S.unbridged(), declContext, mustBeExpr);
}

BridgedStringLiteralExpr
BridgedStringLiteralExpr_createParsed(BridgedASTContext cContext,
                                      BridgedStringRef cStr,
                                      BridgedSourceLoc cTokenLoc) {
  ASTContext &context = cContext.unbridged();
  auto str = context.AllocateCopy(cStr.unbridged());
  return new (context) StringLiteralExpr(str, cTokenLoc.unbridged());
}

BridgedTapExpr BridgedTapExpr_create(BridgedASTContext cContext,
                                     BridgedBraceStmt cBody) {
  return new (cContext.unbridged()) TapExpr(nullptr, cBody.unbridged());
}

BridgedTernaryExpr BridgedTernaryExpr_createParsed(
    BridgedASTContext cContext, BridgedSourceLoc cQuestionLoc,
    BridgedExpr cThenExpr, BridgedSourceLoc cColonLoc) {
  return new (cContext.unbridged()) TernaryExpr(
      cQuestionLoc.unbridged(), cThenExpr.unbridged(), cColonLoc.unbridged());
}

BridgedTryExpr BridgedTryExpr_createParsed(BridgedASTContext cContext,
                                           BridgedSourceLoc cTryLoc,
                                           BridgedExpr cSubExpr) {
  return new (cContext.unbridged())
      TryExpr(cTryLoc.unbridged(), cSubExpr.unbridged());
}

BridgedTupleExpr BridgedTupleExpr_createParsed(BridgedASTContext cContext,
                                               BridgedSourceLoc cLParen,
                                               BridgedArrayRef subs,
                                               BridgedArrayRef names,
                                               BridgedArrayRef cNameLocs,
                                               BridgedSourceLoc cRParen) {
  ASTContext &context = cContext.unbridged();
  return TupleExpr::create(
      context, cLParen.unbridged(), subs.unbridged<Expr *>(),
      names.unbridged<Identifier>(), cNameLocs.unbridged<SourceLoc>(),
      cRParen.unbridged(), /*Implicit*/ false);
}

BridgedTupleExpr BridgedTupleExpr_createParsedDictionaryElement(
    BridgedASTContext cContext, BridgedExpr cKeyExpr, BridgedExpr cValueExpr) {
  return TupleExpr::createImplicit(
      cContext.unbridged(), {cKeyExpr.unbridged(), cValueExpr.unbridged()}, {});
}

BridgedTypeExpr BridgedTypeExpr_createParsed(BridgedASTContext cContext,
                                             BridgedTypeRepr cType) {
  ASTContext &context = cContext.unbridged();
  return new (context) TypeExpr(cType.unbridged());
}

BridgedUnresolvedDeclRefExpr BridgedUnresolvedDeclRefExpr_createParsed(
    BridgedASTContext cContext, BridgedDeclNameRef cName,
    BridgedDeclRefKind cKind, BridgedDeclNameLoc cLoc) {
  DeclRefKind kind;
  switch (cKind) {
  case BridgedDeclRefKindOrdinary:
    kind = DeclRefKind::Ordinary;
    break;
  case BridgedDeclRefKindBinaryOperator:
    kind = DeclRefKind::BinaryOperator;
    break;
  case BridgedDeclRefKindPostfixOperator:
    kind = DeclRefKind::PostfixOperator;
    break;
  case BridgedDeclRefKindPrefixOperator:
    kind = DeclRefKind::PrefixOperator;
    break;
  }
  return new (cContext.unbridged())
      UnresolvedDeclRefExpr(cName.unbridged(), kind, cLoc.unbridged());
}

BridgedUnresolvedDotExpr BridgedUnresolvedDotExpr_createParsed(
    BridgedASTContext cContext, BridgedExpr base, BridgedSourceLoc cDotLoc,
    BridgedDeclNameRef cName, BridgedDeclNameLoc cNameLoc) {
  ASTContext &context = cContext.unbridged();
  return new (context) UnresolvedDotExpr(
      base.unbridged(), cDotLoc.unbridged(), cName.unbridged(),
      cNameLoc.unbridged(), /*isImplicit=*/false);
}

BridgedUnresolvedMemberExpr BridgedUnresolvedMemberExpr_createParsed(
    BridgedASTContext cContext, BridgedSourceLoc cDotLoc,
    BridgedDeclNameRef cName, BridgedDeclNameLoc cNameLoc) {
  ASTContext &context = cContext.unbridged();
  return new (context)
      UnresolvedMemberExpr(cDotLoc.unbridged(), cNameLoc.unbridged(),
                           cName.unbridged(), /*isImplicit=*/false);
}

BridgedUnresolvedPatternExpr
BridgedUnresolvedPatternExpr_createParsed(BridgedASTContext cContext,
                                          BridgedPattern cPattern) {
  return new (cContext.unbridged()) UnresolvedPatternExpr(cPattern.unbridged());
}

void BridgedExpr_setImplicit(BridgedExpr cExpr) {
  cExpr.unbridged()->setImplicit();
}

//===----------------------------------------------------------------------===//
// MARK: Stmts
//===----------------------------------------------------------------------===//

BridgedStmtConditionElement
BridgedStmtConditionElement_createBoolean(BridgedExpr expr) {
  return StmtConditionElement(expr.unbridged());
}

BridgedStmtConditionElement BridgedStmtConditionElement_createPatternBinding(
    BridgedASTContext cContext, BridgedSourceLoc cIntroducerLoc,
    BridgedPattern cPattern, BridgedExpr cInitializer) {
  return StmtConditionElement(ConditionalPatternBindingInfo::create(
      cContext.unbridged(), cIntroducerLoc.unbridged(), cPattern.unbridged(),
      cInitializer.unbridged()));
}

BridgedBraceStmt BridgedBraceStmt_createParsed(BridgedASTContext cContext,
                                               BridgedSourceLoc cLBLoc,
                                               BridgedArrayRef elements,
                                               BridgedSourceLoc cRBLoc) {
  llvm::SmallVector<ASTNode, 6> nodes;
  for (auto node : elements.unbridged<BridgedASTNode>()) {
    if (node.Kind == ASTNodeKindExpr) {
      auto expr = (Expr *)node.Raw;
      nodes.push_back(expr);
    } else if (node.Kind == ASTNodeKindStmt) {
      auto stmt = (Stmt *)node.Raw;
      nodes.push_back(stmt);
    } else {
      assert(node.Kind == ASTNodeKindDecl);
      auto decl = (Decl *)node.Raw;
      nodes.push_back(decl);

      // Variable declarations are part of the list on par with pattern binding
      // declarations per the legacy parser.
      if (auto *bindingDecl = dyn_cast<PatternBindingDecl>(decl)) {
        for (auto i : range(bindingDecl->getNumPatternEntries())) {
          bindingDecl->getPattern(i)->forEachVariable(
              [&nodes](VarDecl *variable) { nodes.push_back(variable); });
        }
      }
    }
  }

  ASTContext &context = cContext.unbridged();
  return BraceStmt::create(context, cLBLoc.unbridged(), nodes,
                           cRBLoc.unbridged());
}

BridgedBreakStmt BridgedBreakStmt_createParsed(BridgedDeclContext cDeclContext,
                                               BridgedSourceLoc cLoc,
                                               BridgedIdentifier cTargetName,
                                               BridgedSourceLoc cTargetLoc) {
  return new (cDeclContext.unbridged()->getASTContext())
      BreakStmt(cLoc.unbridged(), cTargetName.unbridged(),
                cTargetLoc.unbridged(), cDeclContext.unbridged());
}

void getCaseLabelItems(BridgedArrayRef cItems,
                       SmallVectorImpl<CaseLabelItem> &output) {
  for (auto &elem : cItems.unbridged<BridgedCaseLabelItemInfo>()) {
    if (!elem.IsDefault) {
      output.emplace_back(elem.ThePattern.unbridged(),
                          elem.WhereLoc.unbridged(),
                          elem.GuardExpr.unbridged());
    } else {
      output.push_back(CaseLabelItem::getDefault(
          cast<AnyPattern>(elem.ThePattern.unbridged()),
          elem.WhereLoc.unbridged(), elem.GuardExpr.unbridged()));
    }
  }
}

BridgedCaseStmt BridgedCaseStmt_createParsedSwitchCase(
    BridgedASTContext cContext, BridgedSourceLoc cIntroducerLoc,
    BridgedArrayRef cCaseLabelItems, BridgedSourceLoc cUnknownAttrLoc,
    BridgedSourceLoc cTerminatorLoc, BridgedBraceStmt cBody) {
  SmallVector<CaseLabelItem, 1> labelItems;
  getCaseLabelItems(cCaseLabelItems, labelItems);

  return CaseStmt::createParsedSwitchCase(
      cContext.unbridged(), cIntroducerLoc.unbridged(), labelItems,
      cUnknownAttrLoc.unbridged(), cTerminatorLoc.unbridged(),
      cBody.unbridged());
}

BridgedCaseStmt BridgedCaseStmt_createParsedDoCatch(
    BridgedASTContext cContext, BridgedSourceLoc cCatchLoc,
    BridgedArrayRef cCaseLabelItems, BridgedBraceStmt cBody) {
  SmallVector<CaseLabelItem, 1> labelItems;
  getCaseLabelItems(cCaseLabelItems, labelItems);

  return CaseStmt::createParsedDoCatch(cContext.unbridged(),
                                       cCatchLoc.unbridged(), labelItems,
                                       cBody.unbridged());
}

BridgedContinueStmt BridgedContinueStmt_createParsed(
    BridgedDeclContext cDeclContext, BridgedSourceLoc cLoc,
    BridgedIdentifier cTargetName, BridgedSourceLoc cTargetLoc) {
  return new (cDeclContext.unbridged()->getASTContext())
      ContinueStmt(cLoc.unbridged(), cTargetName.unbridged(),
                   cTargetLoc.unbridged(), cDeclContext.unbridged());
}

BridgedDeferStmt BridgedDeferStmt_createParsed(BridgedDeclContext cDeclContext,
                                               BridgedSourceLoc cDeferLoc) {
  return DeferStmt::create(cDeclContext.unbridged(), cDeferLoc.unbridged());
}

BridgedFuncDecl BridgedDeferStmt_getTempDecl(BridgedDeferStmt bridged) {
  return bridged.unbridged()->getTempDecl();
}

BridgedDiscardStmt BridgedDiscardStmt_createParsed(BridgedASTContext cContext,
                                                   BridgedSourceLoc cDiscardLoc,
                                                   BridgedExpr cSubExpr) {
  return new (cContext.unbridged())
      DiscardStmt(cDiscardLoc.unbridged(), cSubExpr.unbridged());
}

BridgedDoStmt BridgedDoStmt_createParsed(BridgedASTContext cContext,
                                         BridgedLabeledStmtInfo cLabelInfo,
                                         BridgedSourceLoc cDoLoc,
                                         BridgedBraceStmt cBody) {
  return new (cContext.unbridged())
      DoStmt(cLabelInfo.unbridged(), cDoLoc.unbridged(), cBody.unbridged());
}

BridgedDoCatchStmt BridgedDoCatchStmt_createParsed(
    BridgedDeclContext cDeclContext, BridgedLabeledStmtInfo cLabelInfo,
    BridgedSourceLoc cDoLoc, BridgedSourceLoc cThrowsLoc,
    BridgedNullableTypeRepr cThrownType, BridgedStmt cBody,
    BridgedArrayRef cCatches) {
  return DoCatchStmt::create(cDeclContext.unbridged(), cLabelInfo.unbridged(),
                             cDoLoc.unbridged(), cThrowsLoc.unbridged(),
                             cThrownType.unbridged(), cBody.unbridged(),
                             cCatches.unbridged<CaseStmt *>());
}

BridgedFallthroughStmt
BridgedFallthroughStmt_createParsed(BridgedASTContext cContext,
                                    BridgedSourceLoc cLoc) {
  return new (cContext.unbridged()) FallthroughStmt(cLoc.unbridged());
}

BridgedForEachStmt BridgedForEachStmt_createParsed(
    BridgedASTContext cContext, BridgedLabeledStmtInfo cLabelInfo,
    BridgedSourceLoc cForLoc, BridgedSourceLoc cTryLoc,
    BridgedSourceLoc cAwaitLoc, BridgedPattern cPat, BridgedSourceLoc cInLoc,
    BridgedExpr cSequence, BridgedSourceLoc cWhereLoc,
    BridgedNullableExpr cWhereExpr, BridgedBraceStmt cBody) {
  return new (cContext.unbridged()) ForEachStmt(
      cLabelInfo.unbridged(), cForLoc.unbridged(), cTryLoc.unbridged(),
      cAwaitLoc.unbridged(), cPat.unbridged(), cInLoc.unbridged(),
      cSequence.unbridged(), cWhereLoc.unbridged(), cWhereExpr.unbridged(),
      cBody.unbridged());
}

BridgedGuardStmt BridgedGuardStmt_createParsed(BridgedASTContext cContext,
                                               BridgedSourceLoc cGuardLoc,
                                               BridgedArrayRef cConds,
                                               BridgedBraceStmt cBody) {
  auto &context = cContext.unbridged();
  StmtCondition cond = context.AllocateTransform<StmtConditionElement>(
      cConds.unbridged<BridgedStmtConditionElement>(),
      [](auto &e) { return e.unbridged(); });

  return new (context)
      GuardStmt(cGuardLoc.unbridged(), cond, cBody.unbridged());
}

BridgedIfStmt BridgedIfStmt_createParsed(
    BridgedASTContext cContext, BridgedLabeledStmtInfo cLabelInfo,
    BridgedSourceLoc cIfLoc, BridgedArrayRef cConds, BridgedBraceStmt cThen,
    BridgedSourceLoc cElseLoc, BridgedNullableStmt cElse) {
  auto &context = cContext.unbridged();
  StmtCondition cond = context.AllocateTransform<StmtConditionElement>(
      cConds.unbridged<BridgedStmtConditionElement>(),
      [](auto &e) { return e.unbridged(); });

  return new (context)
      IfStmt(cLabelInfo.unbridged(), cIfLoc.unbridged(), cond,
             cThen.unbridged(), cElseLoc.unbridged(), cElse.unbridged());
}

BridgedRepeatWhileStmt BridgedRepeatWhileStmt_createParsed(
    BridgedASTContext cContext, BridgedLabeledStmtInfo cLabelInfo,
    BridgedSourceLoc cRepeatLoc, BridgedExpr cCond, BridgedSourceLoc cWhileLoc,
    BridgedStmt cBody) {
  return new (cContext.unbridged()) RepeatWhileStmt(
      cLabelInfo.unbridged(), cRepeatLoc.unbridged(), cCond.unbridged(),
      cWhileLoc.unbridged(), cBody.unbridged());
}

BridgedReturnStmt BridgedReturnStmt_createParsed(BridgedASTContext cContext,
                                                 BridgedSourceLoc cLoc,
                                                 BridgedNullableExpr expr) {
  ASTContext &context = cContext.unbridged();
  return ReturnStmt::createParsed(context, cLoc.unbridged(), expr.unbridged());
}

BridgedSwitchStmt BridgedSwitchStmt_createParsed(
    BridgedASTContext cContext, BridgedLabeledStmtInfo cLabelInfo,
    BridgedSourceLoc cSwitchLoc, BridgedExpr cSubjectExpr,
    BridgedSourceLoc cLBraceLoc, BridgedArrayRef cCases,
    BridgedSourceLoc cRBraceLoc) {
  auto &context = cContext.unbridged();
  auto cases =
      context.AllocateTransform<ASTNode>(cCases.unbridged<BridgedASTNode>(),
                                         [](auto &e) { return e.unbridged(); });
  return SwitchStmt::create(cLabelInfo.unbridged(), cSwitchLoc.unbridged(),
                            cSubjectExpr.unbridged(), cLBraceLoc.unbridged(),
                            cases, cRBraceLoc.unbridged(),
                            cRBraceLoc.unbridged(), cContext.unbridged());
}

BridgedThenStmt BridgedThenStmt_createParsed(BridgedASTContext cContext,
                                             BridgedSourceLoc cThenLoc,
                                             BridgedExpr cResult) {
  return ThenStmt::createParsed(cContext.unbridged(), cThenLoc.unbridged(),
                                cResult.unbridged());
}

BridgedThrowStmt BridgedThrowStmt_createParsed(BridgedASTContext cContext,
                                               BridgedSourceLoc cThrowLoc,
                                               BridgedExpr cSubExpr) {
  return new (cContext.unbridged())
      ThrowStmt(cThrowLoc.unbridged(), cSubExpr.unbridged());
}

BridgedWhileStmt BridgedWhileStmt_createParsed(
    BridgedASTContext cContext, BridgedLabeledStmtInfo cLabelInfo,
    BridgedSourceLoc cWhileLoc, BridgedArrayRef cCond, BridgedStmt cBody) {
  auto &context = cContext.unbridged();
  StmtCondition cond = context.AllocateTransform<StmtConditionElement>(
      cCond.unbridged<BridgedStmtConditionElement>(),
      [](auto &e) { return e.unbridged(); });

  return new (cContext.unbridged()) WhileStmt(
      cLabelInfo.unbridged(), cWhileLoc.unbridged(), cond, cBody.unbridged());
}

BridgedYieldStmt BridgedYieldStmt_createParsed(BridgedASTContext cContext,
                                               BridgedSourceLoc cYieldLoc,
                                               BridgedSourceLoc cLParenLoc,
                                               BridgedArrayRef cYields,
                                               BridgedSourceLoc cRParenLoc) {
  return YieldStmt::create(cContext.unbridged(), cYieldLoc.unbridged(),
                           cLParenLoc.unbridged(), cYields.unbridged<Expr *>(),
                           cRParenLoc.unbridged());
}

//===----------------------------------------------------------------------===//
// MARK: TypeAttributes
//===----------------------------------------------------------------------===//

BridgedTypeAttrKind BridgedTypeAttrKind_fromString(BridgedStringRef cStr) {
  auto optKind = TypeAttribute::getAttrKindFromString(cStr.unbridged());
  if (!optKind)
    return BridgedTypeAttrKindNone;
  switch (*optKind) {
#define TYPE_ATTR(_, CLASS)                                                    \
  case TypeAttrKind::CLASS:                                                    \
    return BridgedTypeAttrKind##CLASS;
#include "swift/AST/TypeAttr.def"
  }
}

static std::optional<TypeAttrKind> unbridged(BridgedTypeAttrKind kind) {
  switch (kind) {
#define TYPE_ATTR(_, CLASS)                                                    \
  case BridgedTypeAttrKind##CLASS:                                             \
    return TypeAttrKind::CLASS;
#include "swift/AST/TypeAttr.def"
  case BridgedTypeAttrKindNone:
    return std::nullopt;
  }
  llvm_unreachable("unhandled enum value");
}

namespace swift {
class TypeAttributes {
public:
  SmallVector<TypeOrCustomAttr> attrs;
  TypeAttributes() {}
};
} // namespace swift

BridgedTypeAttributes BridgedTypeAttributes_create() {
  return new TypeAttributes();
}

void BridgedTypeAttributes_delete(BridgedTypeAttributes cAttributes) {
  delete cAttributes.unbridged();
}

void BridgedTypeAttributes_add(BridgedTypeAttributes cAttributes,
                               BridgedTypeAttribute cAttribute) {
  cAttributes.unbridged()->attrs.push_back(cAttribute.unbridged());
}

bool BridgedTypeAttributes_isEmpty(BridgedTypeAttributes cAttributes) {
  TypeAttributes *typeAttributes = cAttributes.unbridged();
  return typeAttributes->attrs.empty();
}

BridgedTypeAttribute BridgedTypeAttribute_createSimple(
    BridgedASTContext cContext, BridgedTypeAttrKind cKind,
    BridgedSourceLoc cAtLoc, BridgedSourceLoc cNameLoc) {
  auto optKind = unbridged(cKind);
  assert(optKind && "creating attribute of invalid kind?");
  return TypeAttribute::createSimple(cContext.unbridged(), *optKind,
                                     cAtLoc.unbridged(), cNameLoc.unbridged());
}

BridgedTypeAttribute BridgedTypeAttribute_createIsolated(
    BridgedASTContext cContext,
    BridgedSourceLoc cAtLoc, BridgedSourceLoc cNameLoc,
    BridgedSourceLoc cLPLoc, BridgedSourceLoc cIsolationLoc,
    BridgedIsolatedTypeAttrIsolationKind cIsolation, BridgedSourceLoc cRPLoc) {
  auto isolationKind = [=] {
    switch (cIsolation) {
    case BridgedIsolatedTypeAttrIsolationKind_DynamicIsolation:
      return IsolatedTypeAttr::IsolationKind::Dynamic;
    }
    llvm_unreachable("bad kind");
  }();
  return new (cContext.unbridged()) IsolatedTypeAttr(cAtLoc.unbridged(),
                                                     cNameLoc.unbridged(),
                                                     {cLPLoc.unbridged(),
                                                      cRPLoc.unbridged()},
                                                     {isolationKind,
                                                      cIsolationLoc.unbridged()});
}

//===----------------------------------------------------------------------===//
// MARK: TypeReprs
//===----------------------------------------------------------------------===//

BridgedUnqualifiedIdentTypeRepr BridgedUnqualifiedIdentTypeRepr_createParsed(
    BridgedASTContext cContext, BridgedSourceLoc cLoc, BridgedIdentifier id) {
  return UnqualifiedIdentTypeRepr::create(cContext.unbridged(),
                                          DeclNameLoc(cLoc.unbridged()),
                                          DeclNameRef(id.unbridged()));
}

BridgedUnqualifiedIdentTypeRepr BridgedUnqualifiedIdentTypeRepr_createParsed(
    BridgedASTContext cContext, BridgedIdentifier name,
    BridgedSourceLoc cNameLoc, BridgedArrayRef genericArgs,
    BridgedSourceLoc cLAngleLoc, BridgedSourceLoc cRAngleLoc) {
  ASTContext &context = cContext.unbridged();
  auto Loc = DeclNameLoc(cNameLoc.unbridged());
  auto Name = DeclNameRef(name.unbridged());
  SourceLoc lAngleLoc = cLAngleLoc.unbridged();
  SourceLoc rAngleLoc = cRAngleLoc.unbridged();
  return UnqualifiedIdentTypeRepr::create(context, Loc, Name,
                                          genericArgs.unbridged<TypeRepr *>(),
                                          SourceRange{lAngleLoc, rAngleLoc});
}

BridgedOptionalTypeRepr
BridgedOptionalTypeRepr_createParsed(BridgedASTContext cContext,
                                     BridgedTypeRepr base,
                                     BridgedSourceLoc cQuestionLoc) {
  ASTContext &context = cContext.unbridged();
  return new (context)
      OptionalTypeRepr(base.unbridged(), cQuestionLoc.unbridged());
}

BridgedImplicitlyUnwrappedOptionalTypeRepr
BridgedImplicitlyUnwrappedOptionalTypeRepr_createParsed(
    BridgedASTContext cContext, BridgedTypeRepr base,
    BridgedSourceLoc cExclamationLoc) {
  ASTContext &context = cContext.unbridged();
  return new (context) ImplicitlyUnwrappedOptionalTypeRepr(
      base.unbridged(), cExclamationLoc.unbridged());
}

BridgedArrayTypeRepr BridgedArrayTypeRepr_createParsed(
    BridgedASTContext cContext, BridgedTypeRepr base,
    BridgedSourceLoc cLSquareLoc, BridgedSourceLoc cRSquareLoc) {
  ASTContext &context = cContext.unbridged();
  SourceLoc lSquareLoc = cLSquareLoc.unbridged();
  SourceLoc rSquareLoc = cRSquareLoc.unbridged();
  return new (context)
      ArrayTypeRepr(base.unbridged(), SourceRange{lSquareLoc, rSquareLoc});
}

BridgedDictionaryTypeRepr BridgedDictionaryTypeRepr_createParsed(
    BridgedASTContext cContext, BridgedSourceLoc cLSquareLoc,
    BridgedTypeRepr keyType, BridgedSourceLoc cColonloc,
    BridgedTypeRepr valueType, BridgedSourceLoc cRSquareLoc) {
  ASTContext &context = cContext.unbridged();
  SourceLoc lSquareLoc = cLSquareLoc.unbridged();
  SourceLoc colonLoc = cColonloc.unbridged();
  SourceLoc rSquareLoc = cRSquareLoc.unbridged();
  return new (context)
      DictionaryTypeRepr(keyType.unbridged(), valueType.unbridged(), colonLoc,
                         SourceRange{lSquareLoc, rSquareLoc});
}

BridgedInverseTypeRepr
BridgedInverseTypeRepr_createParsed(BridgedASTContext cContext,
                                    BridgedSourceLoc cTildeLoc,
                                    BridgedTypeRepr cConstraint) {

  return new (cContext.unbridged())
      InverseTypeRepr(cTildeLoc.unbridged(), cConstraint.unbridged());
}

BridgedMetatypeTypeRepr
BridgedMetatypeTypeRepr_createParsed(BridgedASTContext cContext,
                                     BridgedTypeRepr baseType,
                                     BridgedSourceLoc cTypeLoc) {
  ASTContext &context = cContext.unbridged();
  SourceLoc tyLoc = cTypeLoc.unbridged();
  return new (context) MetatypeTypeRepr(baseType.unbridged(), tyLoc);
}

BridgedProtocolTypeRepr
BridgedProtocolTypeRepr_createParsed(BridgedASTContext cContext,
                                     BridgedTypeRepr baseType,
                                     BridgedSourceLoc cProtoLoc) {
  ASTContext &context = cContext.unbridged();
  SourceLoc protoLoc = cProtoLoc.unbridged();
  return new (context) ProtocolTypeRepr(baseType.unbridged(), protoLoc);
}

BridgedPackElementTypeRepr
BridgedPackElementTypeRepr_createParsed(BridgedASTContext cContext,
                                        BridgedTypeRepr base,
                                        BridgedSourceLoc cEachLoc) {
  ASTContext &context = cContext.unbridged();
  return new (context)
      PackElementTypeRepr(cEachLoc.unbridged(), base.unbridged());
}

BridgedPackExpansionTypeRepr
BridgedPackExpansionTypeRepr_createParsed(BridgedASTContext cContext,
                                          BridgedTypeRepr base,
                                          BridgedSourceLoc cRepeatLoc) {
  ASTContext &context = cContext.unbridged();
  return new (context)
      PackExpansionTypeRepr(cRepeatLoc.unbridged(), base.unbridged());
}

BridgedAttributedTypeRepr
BridgedAttributedTypeRepr_createParsed(BridgedASTContext cContext,
                                       BridgedTypeRepr base,
                                       BridgedTypeAttributes cAttributes) {
  TypeAttributes *typeAttributes = cAttributes.unbridged();
  assert(!typeAttributes->attrs.empty());

  auto attributedType = AttributedTypeRepr::create(
      cContext.unbridged(), typeAttributes->attrs, base.unbridged());
  delete typeAttributes;
  return attributedType;
}

BridgedSpecifierTypeRepr BridgedSpecifierTypeRepr_createParsed(
    BridgedASTContext cContext, BridgedTypeRepr base,
    BridgedAttributedTypeSpecifier specifier, BridgedSourceLoc cSpecifierLoc) {
  ASTContext &context = cContext.unbridged();
  SourceLoc loc = cSpecifierLoc.unbridged();
  TypeRepr *baseType = base.unbridged();
  switch (specifier) {
  case BridgedAttributedTypeSpecifierInOut: {
    return new (context)
        OwnershipTypeRepr(baseType, ParamSpecifier::InOut, loc);
  }
  case BridgedAttributedTypeSpecifierBorrowing: {
    return new (context)
        OwnershipTypeRepr(baseType, ParamSpecifier::Borrowing, loc);
  }
  case BridgedAttributedTypeSpecifierConsuming: {
    return new (context)
        OwnershipTypeRepr(baseType, ParamSpecifier::Consuming, loc);
  }
  case BridgedAttributedTypeSpecifierLegacyShared: {
    return new (context)
        OwnershipTypeRepr(baseType, ParamSpecifier::LegacyShared, loc);
  }
  case BridgedAttributedTypeSpecifierLegacyOwned: {
    return new (context)
        OwnershipTypeRepr(baseType, ParamSpecifier::LegacyOwned, loc);
  }
  case BridgedAttributedTypeSpecifierTransferring: {
    return new (context) TransferringTypeRepr(baseType, loc);
  }
  case BridgedAttributedTypeSpecifierSending: {
    return new (context) SendingTypeRepr(baseType, loc);
  }
  case BridgedAttributedTypeSpecifierConst: {
    return new (context) CompileTimeConstTypeRepr(baseType, loc);
  }
  case BridgedAttributedTypeSpecifierIsolated: {
    return new (context) IsolatedTypeRepr(baseType, loc);
  }
  }
}

BridgedVarargTypeRepr
BridgedVarargTypeRepr_createParsed(BridgedASTContext cContext,
                                   BridgedTypeRepr base,
                                   BridgedSourceLoc cEllipsisLoc) {
  ASTContext &context = cContext.unbridged();
  SourceLoc ellipsisLoc = cEllipsisLoc.unbridged();
  TypeRepr *baseType = base.unbridged();
  return new (context) VarargTypeRepr(baseType, ellipsisLoc);
}

BridgedTupleTypeRepr BridgedTupleTypeRepr_createParsed(
    BridgedASTContext cContext, BridgedArrayRef elements,
    BridgedSourceLoc cLParenLoc, BridgedSourceLoc cRParenLoc) {
  ASTContext &context = cContext.unbridged();
  SourceLoc lParen = cLParenLoc.unbridged();
  SourceLoc rParen = cRParenLoc.unbridged();

  SmallVector<TupleTypeReprElement, 8> tupleElements;
  for (auto element : elements.unbridged<BridgedTupleTypeElement>()) {
    TupleTypeReprElement elementRepr;
    elementRepr.Name = element.Name.unbridged();
    elementRepr.NameLoc = element.NameLoc.unbridged();
    elementRepr.SecondName = element.SecondName.unbridged();
    elementRepr.SecondNameLoc = element.SecondNameLoc.unbridged();
    elementRepr.UnderscoreLoc = element.UnderscoreLoc.unbridged();
    elementRepr.ColonLoc = element.ColonLoc.unbridged();
    elementRepr.Type = element.Type.unbridged();
    elementRepr.TrailingCommaLoc = element.TrailingCommaLoc.unbridged();
    tupleElements.emplace_back(elementRepr);
  }

  return TupleTypeRepr::create(context, tupleElements,
                               SourceRange{lParen, rParen});
}

BridgedDeclRefTypeRepr BridgedDeclRefTypeRepr_createParsed(
    BridgedASTContext cContext, BridgedTypeRepr cBase, BridgedIdentifier cName,
    BridgedSourceLoc cLoc, BridgedArrayRef cGenericArguments,
    BridgedSourceRange cAngleRange) {
  ASTContext &context = cContext.unbridged();
  auto genericArguments = cGenericArguments.unbridged<TypeRepr *>();
  auto angleRange = cAngleRange.unbridged();

  assert(angleRange.isValid() || genericArguments.empty());

  return DeclRefTypeRepr::create(
      context, cBase.unbridged(), DeclNameLoc(cLoc.unbridged()),
      DeclNameRef(cName.unbridged()), genericArguments, angleRange);
}

BridgedCompositionTypeRepr
BridgedCompositionTypeRepr_createEmpty(BridgedASTContext cContext,
                                       BridgedSourceLoc cAnyLoc) {
  ASTContext &context = cContext.unbridged();
  SourceLoc anyLoc = cAnyLoc.unbridged();
  return CompositionTypeRepr::createEmptyComposition(context, anyLoc);
}

BridgedCompositionTypeRepr
BridgedCompositionTypeRepr_createParsed(BridgedASTContext cContext,
                                        BridgedArrayRef cTypes,
                                        BridgedSourceLoc cFirstAmpLoc) {
  ASTContext &context = cContext.unbridged();
  SourceLoc firstAmpLoc = cFirstAmpLoc.unbridged();
  auto types = cTypes.unbridged<TypeRepr *>();
  return CompositionTypeRepr::create(
      context, types, types.front()->getStartLoc(),
      SourceRange{firstAmpLoc, types.back()->getEndLoc()});
}

BridgedFunctionTypeRepr BridgedFunctionTypeRepr_createParsed(
    BridgedASTContext cContext, BridgedTypeRepr argsTy,
    BridgedSourceLoc cAsyncLoc, BridgedSourceLoc cThrowsLoc,
    BridgedNullableTypeRepr thrownType, BridgedSourceLoc cArrowLoc,
    BridgedTypeRepr resultType) {
  ASTContext &context = cContext.unbridged();
  return new (context) FunctionTypeRepr(
      nullptr, cast<TupleTypeRepr>(argsTy.unbridged()), cAsyncLoc.unbridged(),
      cThrowsLoc.unbridged(), thrownType.unbridged(), cArrowLoc.unbridged(),
      resultType.unbridged());
}

BridgedNamedOpaqueReturnTypeRepr
BridgedNamedOpaqueReturnTypeRepr_createParsed(BridgedASTContext cContext,
                                              BridgedTypeRepr baseTy) {
  ASTContext &context = cContext.unbridged();
  return new (context) NamedOpaqueReturnTypeRepr(baseTy.unbridged(), nullptr);
}

BridgedOpaqueReturnTypeRepr
BridgedOpaqueReturnTypeRepr_createParsed(BridgedASTContext cContext,
                                         BridgedSourceLoc cOpaqueLoc,
                                         BridgedTypeRepr baseTy) {
  ASTContext &context = cContext.unbridged();
  return new (context)
      OpaqueReturnTypeRepr(cOpaqueLoc.unbridged(), baseTy.unbridged());
}

BridgedExistentialTypeRepr
BridgedExistentialTypeRepr_createParsed(BridgedASTContext cContext,
                                        BridgedSourceLoc cAnyLoc,
                                        BridgedTypeRepr baseTy) {
  ASTContext &context = cContext.unbridged();
  return new (context)
      ExistentialTypeRepr(cAnyLoc.unbridged(), baseTy.unbridged());
}

//===----------------------------------------------------------------------===//
// MARK: Patterns
//===----------------------------------------------------------------------===//

BridgedNullableVarDecl BridgedPattern_getSingleVar(BridgedPattern cPattern) {
  return cPattern.unbridged()->getSingleVar();
}

BridgedAnyPattern BridgedAnyPattern_createParsed(BridgedASTContext cContext,
                                                 BridgedSourceLoc cLoc) {
  return new (cContext.unbridged()) AnyPattern(cLoc.unbridged());
}

BridgedBindingPattern
BridgedBindingPattern_createParsed(BridgedASTContext cContext,
                                   BridgedSourceLoc cKeywordLoc, bool isLet,
                                   BridgedPattern cSubPattern) {
  VarDecl::Introducer introducer =
      isLet ? VarDecl::Introducer::Let : VarDecl::Introducer::Var;
  return BindingPattern::createParsed(cContext.unbridged(),
                                      cKeywordLoc.unbridged(), introducer,
                                      cSubPattern.unbridged());
}

BridgedBindingPattern
BridgedBindingPattern_createImplicitCatch(BridgedDeclContext cDeclContext,
                                          BridgedSourceLoc cLoc) {
  return BindingPattern::createImplicitCatch(cDeclContext.unbridged(),
                                             cLoc.unbridged());
}

BridgedExprPattern
BridgedExprPattern_createParsed(BridgedDeclContext cDeclContext,
                                BridgedExpr cExpr) {
  auto *DC = cDeclContext.unbridged();
  auto &context = DC->getASTContext();
  return ExprPattern::createParsed(context, cExpr.unbridged(), DC);
}

BridgedIsPattern BridgedIsPattern_createParsed(BridgedASTContext cContext,
                                               BridgedSourceLoc cIsLoc,
                                               BridgedTypeExpr cTypeExpr) {
  return new (cContext.unbridged())
      IsPattern(cIsLoc.unbridged(), cTypeExpr.unbridged(),
                /*subPattern=*/nullptr, CheckedCastKind::Unresolved);
}

BridgedNamedPattern
BridgedNamedPattern_createParsed(BridgedASTContext cContext,
                                 BridgedDeclContext cDeclContext,
                                 BridgedIdentifier name, BridgedSourceLoc loc) {
  auto &context = cContext.unbridged();
  auto *dc = cDeclContext.unbridged();

  // Note 'isStatic' and the introducer value are temporary.
  // The outer context should set the correct values.
  auto *varDecl = new (context) VarDecl(
      /*isStatic=*/false, VarDecl::Introducer::Let, loc.unbridged(),
      name.unbridged(), dc);
  auto *pattern = new (context) NamedPattern(varDecl);
  return pattern;
}

BridgedParenPattern BridgedParenPattern_createParsed(
    BridgedASTContext cContext, BridgedSourceLoc cLParenLoc,
    BridgedPattern cSubPattern, BridgedSourceLoc cRParenLoc) {
  return new (cContext.unbridged()) ParenPattern(
      cLParenLoc.unbridged(), cSubPattern.unbridged(), cRParenLoc.unbridged());
}

BridgedTuplePattern BridgedTuplePattern_createParsed(
    BridgedASTContext cContext, BridgedSourceLoc cLParenLoc,
    BridgedArrayRef cElements, BridgedSourceLoc cRParenLoc) {
  ASTContext &context = cContext.unbridged();
  llvm::SmallVector<TuplePatternElt, 4> elements;
  elements.reserve(cElements.Length);
  llvm::transform(cElements.unbridged<BridgedTuplePatternElt>(),
                  elements.begin(), [](const BridgedTuplePatternElt &elt) {
                    return TuplePatternElt(elt.Label.unbridged(),
                                           elt.LabelLoc.unbridged(),
                                           elt.ThePattern.unbridged());
                  });

  return TuplePattern::create(context, cLParenLoc.unbridged(), elements,
                              cRParenLoc.unbridged());
}

BridgedTypedPattern BridgedTypedPattern_createParsed(BridgedASTContext cContext,
                                                     BridgedPattern cPattern,
                                                     BridgedTypeRepr cType) {
  return new (cContext.unbridged())
      TypedPattern(cPattern.unbridged(), cType.unbridged());
}

BridgedTypedPattern
BridgedTypedPattern_createPropagated(BridgedASTContext cContext,
                                     BridgedPattern cPattern,
                                     BridgedTypeRepr cType) {
  return TypedPattern::createPropagated(
      cContext.unbridged(), cPattern.unbridged(), cType.unbridged());
}

void BridgedPattern_setImplicit(BridgedPattern cPattern) {
  cPattern.unbridged()->setImplicit();
}

BridgedIdentifier BridgedPattern_getBoundName(BridgedPattern cPattern) {
  return cPattern.unbridged()->getBoundName();
}

//===----------------------------------------------------------------------===//
// MARK: Misc
//===----------------------------------------------------------------------===//

BridgedGenericParamList BridgedGenericParamList_createParsed(
    BridgedASTContext cContext, BridgedSourceLoc cLeftAngleLoc,
    BridgedArrayRef cParameters,
    BridgedNullableTrailingWhereClause bridgedGenericWhereClause,
    BridgedSourceLoc cRightAngleLoc) {
  SourceLoc whereLoc;
  ArrayRef<RequirementRepr> requirements;
  if (auto *genericWhereClause = bridgedGenericWhereClause.unbridged()) {
    whereLoc = genericWhereClause->getWhereLoc();
    requirements = genericWhereClause->getRequirements();
  }

  return GenericParamList::create(
      cContext.unbridged(), cLeftAngleLoc.unbridged(),
      cParameters.unbridged<GenericTypeParamDecl *>(), whereLoc, requirements,
      cRightAngleLoc.unbridged());
}

BridgedGenericTypeParamDecl BridgedGenericTypeParamDecl_createParsed(
    BridgedASTContext cContext, BridgedDeclContext cDeclContext,
    BridgedSourceLoc cEachLoc, BridgedIdentifier cName,
    BridgedSourceLoc cNameLoc, BridgedNullableTypeRepr bridgedInheritedType,
    size_t index) {
  auto eachLoc = cEachLoc.unbridged();
  auto *decl = GenericTypeParamDecl::createParsed(
      cDeclContext.unbridged(), cName.unbridged(), cNameLoc.unbridged(),
      eachLoc, index,
      /*isParameterPack*/ eachLoc.isValid());

  if (auto *inheritedType = bridgedInheritedType.unbridged()) {
    auto entry = InheritedEntry(inheritedType);
    ASTContext &context = cContext.unbridged();
    decl->setInherited(context.AllocateCopy(llvm::ArrayRef(entry)));
  }

  return decl;
}

BridgedTrailingWhereClause
BridgedTrailingWhereClause_createParsed(BridgedASTContext cContext,
                                        BridgedSourceLoc cWhereKeywordLoc,
                                        BridgedArrayRef cRequirements) {
  SmallVector<RequirementRepr> requirements;
  for (auto &cReq : cRequirements.unbridged<BridgedRequirementRepr>()) {
    switch (cReq.Kind) {
    case BridgedRequirementReprKindTypeConstraint:
      requirements.push_back(RequirementRepr::getTypeConstraint(
          cReq.FirstType.unbridged(), cReq.SeparatorLoc.unbridged(),
          cReq.SecondType.unbridged(),
          /*isExpansionPattern*/ false));
      break;
    case BridgedRequirementReprKindSameType:
      requirements.push_back(RequirementRepr::getSameType(
          cReq.FirstType.unbridged(), cReq.SeparatorLoc.unbridged(),
          cReq.SecondType.unbridged(),
          /*isExpansionPattern*/ false));
      break;
    case BridgedRequirementReprKindLayoutConstraint:
      llvm_unreachable("cannot handle layout constraints!");
    }
  }

  SourceLoc whereKeywordLoc = cWhereKeywordLoc.unbridged();
  SourceLoc endLoc;
  if (requirements.empty()) {
    endLoc = whereKeywordLoc;
  } else {
    endLoc = requirements.back().getSourceRange().End;
  }

  return TrailingWhereClause::create(cContext.unbridged(), whereKeywordLoc,
                                     endLoc, requirements);
}

BridgedParameterList BridgedParameterList_createParsed(
    BridgedASTContext cContext, BridgedSourceLoc cLeftParenLoc,
    BridgedArrayRef cParameters, BridgedSourceLoc cRightParenLoc) {
  ASTContext &context = cContext.unbridged();
  return ParameterList::create(context, cLeftParenLoc.unbridged(),
                               cParameters.unbridged<ParamDecl *>(),
                               cRightParenLoc.unbridged());
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

void BridgedTopLevelCodeDecl_dump(void *decl) {
  static_cast<TopLevelCodeDecl *>(decl)->dump(llvm::errs());
}
void BridgedExpr_dump(void *expr) {
  static_cast<Expr *>(expr)->dump(llvm::errs());
}
void BridgedDecl_dump(void *decl) {
  static_cast<Decl *>(decl)->dump(llvm::errs());
}
void BridgedStmt_dump(void *statement) {
  static_cast<Stmt *>(statement)->dump(llvm::errs());
}
void BridgedTypeRepr_dump(void *type) { static_cast<TypeRepr *>(type)->dump(); }

#pragma clang diagnostic pop

//===----------------------------------------------------------------------===//
// MARK: Plugins
//===----------------------------------------------------------------------===//

PluginCapabilityPtr Plugin_getCapability(PluginHandle handle) {
  auto *plugin = static_cast<CompilerPlugin *>(handle);
  return plugin->getCapability();
}

void Plugin_setCapability(PluginHandle handle, PluginCapabilityPtr data) {
  auto *plugin = static_cast<CompilerPlugin *>(handle);
  plugin->setCapability(data);
}

void Plugin_lock(PluginHandle handle) {
  auto *plugin = static_cast<CompilerPlugin *>(handle);
  plugin->lock();
}

void Plugin_unlock(PluginHandle handle) {
  auto *plugin = static_cast<CompilerPlugin *>(handle);
  plugin->unlock();
}

bool Plugin_spawnIfNeeded(PluginHandle handle) {
  auto *plugin = static_cast<CompilerPlugin *>(handle);
  auto error = plugin->spawnIfNeeded();
  bool hadError(error);
  llvm::consumeError(std::move(error));
  return hadError;
}

bool Plugin_sendMessage(PluginHandle handle, const BridgedData data) {
  auto *plugin = static_cast<CompilerPlugin *>(handle);
  StringRef message(data.BaseAddress, data.Length);
  auto error = plugin->sendMessage(message);
  if (error) {
    // FIXME: Pass the error message back to the caller.
    llvm::consumeError(std::move(error));
    //    llvm::handleAllErrors(std::move(error), [](const llvm::ErrorInfoBase
    //    &err) {
    //      llvm::errs() << err.message() << "\n";
    //    });
    return true;
  }
  return false;
}

bool Plugin_waitForNextMessage(PluginHandle handle, BridgedData *out) {
  auto *plugin = static_cast<CompilerPlugin *>(handle);
  auto result = plugin->waitForNextMessage();
  if (!result) {
    // FIXME: Pass the error message back to the caller.
    llvm::consumeError(result.takeError());
    //    llvm::handleAllErrors(result.takeError(), [](const llvm::ErrorInfoBase
    //    &err) {
    //      llvm::errs() << err.message() << "\n";
    //    });
    return true;
  }
  auto &message = result.get();
  auto size = message.size();
  auto outPtr = malloc(size);
  memcpy(outPtr, message.data(), size);
  *out = BridgedData{(const char *)outPtr, size};
  return false;
}
