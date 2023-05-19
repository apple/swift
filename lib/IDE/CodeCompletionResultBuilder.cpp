//===--- CodeCompletionResultBuilder.cpp ----------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "CodeCompletionResultBuilder.h"
#include "CodeCompletionDiagnostics.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/USRGeneration.h"
#include "swift/Basic/LLVM.h"
#include "swift/IDE/CodeCompletionStringPrinter.h"
#include "swift/IDE/Utils.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Comment.h"
#include "clang/Basic/Module.h"
#include "clang/Index/USRGeneration.h"

using namespace swift;
using namespace swift::ide;

static bool shouldCopyAssociatedUSRForDecl(const ValueDecl *VD) {
  // Avoid trying to generate a USR for some declaration types.
  if (isa<GenericTypeParamDecl>(VD))
    return false;
  if (isa<ParamDecl>(VD))
    return false;
  if (isa<ModuleDecl>(VD))
    return false;
  if (VD->hasClangNode() && !VD->getClangDecl())
    return false;

  return true;
}

template <typename FnTy>
static void walkValueDeclAndOverriddenDecls(const Decl *D, const FnTy &Fn) {
  if (auto *VD = dyn_cast<ValueDecl>(D)) {
    Fn(VD);
    walkOverriddenDecls(VD, Fn);
  }
}

static ArrayRef<NullTerminatedStringRef>
copyAssociatedUSRs(llvm::BumpPtrAllocator &Allocator, const Decl *D) {
  llvm::SmallVector<NullTerminatedStringRef, 4> USRs;
  walkValueDeclAndOverriddenDecls(
      D,
      [&](llvm::PointerUnion<const ValueDecl *, const clang::NamedDecl *> OD) {
        llvm::SmallString<128> SS;
        bool Ignored = true;
        if (auto *OVD = OD.dyn_cast<const ValueDecl *>()) {
          if (shouldCopyAssociatedUSRForDecl(OVD)) {
            llvm::raw_svector_ostream OS(SS);
            Ignored = printValueDeclUSR(OVD, OS);
          }
        } else if (auto *OND = OD.dyn_cast<const clang::NamedDecl *>()) {
          Ignored = clang::index::generateUSRForDecl(OND, SS);
        }

        if (!Ignored)
          USRs.emplace_back(SS, Allocator);
      });

  if (!USRs.empty())
    return makeArrayRef(USRs).copy(Allocator);

  return {};
}

void CodeCompletionResultBuilder::addChunkWithText(
    CodeCompletionString::Chunk::ChunkKind Kind, StringRef Text) {
  addChunkWithTextNoCopy(Kind, Text.copy(*Sink.Allocator));
}

CodeCompletionResult *CodeCompletionResultBuilder::takeResult() {
  auto &Allocator = *Sink.Allocator;
  auto *CCS = CodeCompletionString::create(Allocator, Chunks);

  CodeCompletionDiagnosticSeverity ContextFreeDiagnosticSeverity =
      CodeCompletionDiagnosticSeverity::None;
  NullTerminatedStringRef ContextFreeDiagnosticMessage;
  if (ContextFreeNotRecReason != ContextFreeNotRecommendedReason::None) {
    assert(AssociatedDecl != nullptr &&
           "There should be no case where ContextFreeNotRecReason != None && "
           "AssociatedDecl == nulptr");
    // FIXME: We should generate the message lazily.
    if (const auto *VD = dyn_cast_or_null<ValueDecl>(AssociatedDecl)) {
      CodeCompletionDiagnosticSeverity severity;
      SmallString<256> message;
      llvm::raw_svector_ostream messageOS(message);
      if (!getContextFreeCompletionDiagnostics(ContextFreeNotRecReason, VD,
                                               severity, messageOS)) {
        ContextFreeDiagnosticSeverity = severity;
        ContextFreeDiagnosticMessage =
            NullTerminatedStringRef(message, Allocator);
      }
    }
  }

  /// This variable should be initialized by all the switch cases below.
  ContextFreeCodeCompletionResult *ContextFreeResult = nullptr;

  switch (Kind) {
  case CodeCompletionResultKind::Declaration: {
    NullTerminatedStringRef ModuleName;
    if (CurrentModule) {
      if (Sink.LastModule.first == CurrentModule.getOpaqueValue()) {
        ModuleName = Sink.LastModule.second;
      } else {
        if (auto *C = CurrentModule.dyn_cast<const clang::Module *>()) {
          ModuleName =
              NullTerminatedStringRef(C->getFullModuleName(), Allocator);
        } else {
          ModuleName = NullTerminatedStringRef(
              CurrentModule.get<const swift::ModuleDecl *>()->getName().str(),
              Allocator);
        }
        Sink.LastModule.first = CurrentModule.getOpaqueValue();
        Sink.LastModule.second = ModuleName;
      }
    }

    ContextFreeResult = ContextFreeCodeCompletionResult::createDeclResult(
        Sink, CCS, AssociatedDecl, IsAsync, HasAsyncAlternative, ModuleName,
        NullTerminatedStringRef(BriefDocComment, Allocator),
        copyAssociatedUSRs(Allocator, AssociatedDecl), ResultType,
        ContextFreeNotRecReason, ContextFreeDiagnosticSeverity,
        ContextFreeDiagnosticMessage);
    break;
  }

  case CodeCompletionResultKind::Keyword:
    ContextFreeResult = ContextFreeCodeCompletionResult::createKeywordResult(
        Sink, KeywordKind, CCS,
        NullTerminatedStringRef(BriefDocComment, Allocator), ResultType);
    break;
  case CodeCompletionResultKind::BuiltinOperator:
  case CodeCompletionResultKind::Pattern:
    ContextFreeResult =
        ContextFreeCodeCompletionResult::createPatternOrBuiltInOperatorResult(
            Sink, Kind, CCS, CodeCompletionOperatorKind::None, IsAsync,
            NullTerminatedStringRef(BriefDocComment, Allocator), ResultType,
            ContextFreeNotRecReason, ContextFreeDiagnosticSeverity,
            ContextFreeDiagnosticMessage);
    break;
  case CodeCompletionResultKind::Literal:
    assert(LiteralKind.has_value());
    ContextFreeResult = ContextFreeCodeCompletionResult::createLiteralResult(
        Sink, *LiteralKind, CCS, ResultType);
    break;
  }

  if (Sink.shouldProduceContextFreeResults()) {
    // If the sink only intends to store the context free results in the cache,
    // we don't need to compute any contextual properties.
    return new (Allocator) CodeCompletionResult(
        *ContextFreeResult, SemanticContextKind::None, CodeCompletionFlair(),
        /*NumBytesToErase=*/0, CodeCompletionResultTypeRelation::Unrelated,
        ContextualNotRecommendedReason::None);
  } else {
    assert(
        ContextFreeResult != nullptr &&
        "ContextFreeResult should have been constructed by the switch above");

    // We know that the ContextFreeResult has an AST-based type because it was
    // just computed and not read from the cache and
    // Sink.shouldProduceContextFreeResults() is false. So we can pass nullptr
    // for USRTypeContext.
    CodeCompletionResultTypeRelation typeRelation =
        ContextFreeResult->calculateContextualTypeRelation(
            DC, TypeContext, /*usrTypeContext=*/nullptr);
    ContextualNotRecommendedReason notRecommendedReason =
        ContextFreeResult->calculateContextualNotRecommendedReason(
            ContextualNotRecReason, CanCurrDeclContextHandleAsync);

    return new (Allocator) CodeCompletionResult(
        *ContextFreeResult, SemanticContext, Flair, NumBytesToErase,
        typeRelation, notRecommendedReason);
  }
}

void CodeCompletionResultBuilder::finishResult() {
  if (!Cancelled)
    Sink.Results.push_back(takeResult());
}

void CodeCompletionResultBuilder::setAssociatedDecl(const Decl *D) {
  assert(Kind == CodeCompletionResultKind::Declaration);

  AssociatedDecl = D;

  if (auto *ClangD = D->getClangDecl())
    CurrentModule = ClangD->getImportedOwningModule();
  // FIXME: macros
  // FIXME: imported header module

  if (!CurrentModule) {
    ModuleDecl *MD = D->getModuleContext();

    // If this is an underscored cross-import overlay, map it to the underlying
    // module that declares it instead.
    if (ModuleDecl *Declaring = MD->getDeclaringModuleIfCrossImportOverlay())
      MD = Declaring;

    CurrentModule = MD;
  }

  if (D->getAttrs().getDeprecated(D->getASTContext()))
    setContextFreeNotRecommended(ContextFreeNotRecommendedReason::Deprecated);
  else if (D->getAttrs().getSoftDeprecated(D->getASTContext()))
    setContextFreeNotRecommended(
        ContextFreeNotRecommendedReason::SoftDeprecated);

  if (D->getClangNode()) {
    if (auto *ClangD = D->getClangDecl()) {
      const auto &ClangContext = ClangD->getASTContext();
      if (const clang::RawComment *RC =
              ClangContext.getRawCommentForAnyRedecl(ClangD)) {
        setBriefDocComment(RC->getBriefText(ClangContext));
      }
    }
  } else {
    setBriefDocComment(AssociatedDecl->getSemanticBriefComment());
  }
}

void CodeCompletionResultBuilder::addCallArgument(
    Identifier Name, Identifier LocalName, Type Ty, Type ContextTy,
    bool IsVarArg, bool IsInOut, bool IsIUO, bool IsAutoClosure,
    bool UseUnderscoreLabel, bool IsLabeledTrailingClosure, bool HasDefault) {
  ++CurrentNestingLevel;
  using ChunkKind = CodeCompletionString::Chunk::ChunkKind;

  addSimpleChunk(ChunkKind::CallArgumentBegin);

  if (shouldAnnotateResults()) {
    if (!Name.empty() || !LocalName.empty()) {
      llvm::SmallString<16> EscapedKeyword;

      if (!Name.empty()) {
        addChunkWithText(
            CodeCompletionString::Chunk::ChunkKind::CallArgumentName,
            escapeKeyword(Name.str(), false, EscapedKeyword));

        if (!LocalName.empty() && Name != LocalName) {
          addChunkWithTextNoCopy(ChunkKind::Text, " ");
          getLastChunk().setIsAnnotation();
          addChunkWithText(
              ChunkKind::CallArgumentInternalName,
              escapeKeyword(LocalName.str(), false, EscapedKeyword));
          getLastChunk().setIsAnnotation();
        }
      } else {
        assert(!LocalName.empty());
        addChunkWithTextNoCopy(ChunkKind::CallArgumentName, "_");
        getLastChunk().setIsAnnotation();
        addChunkWithTextNoCopy(ChunkKind::Text, " ");
        getLastChunk().setIsAnnotation();
        addChunkWithText(ChunkKind::CallArgumentInternalName,
                         escapeKeyword(LocalName.str(), false, EscapedKeyword));
      }
      addChunkWithTextNoCopy(ChunkKind::CallArgumentColon, ": ");
    }
  } else {
    if (!Name.empty()) {
      llvm::SmallString<16> EscapedKeyword;
      addChunkWithText(CodeCompletionString::Chunk::ChunkKind::CallArgumentName,
                       escapeKeyword(Name.str(), false, EscapedKeyword));
      addChunkWithTextNoCopy(
          CodeCompletionString::Chunk::ChunkKind::CallArgumentColon, ": ");
    } else if (UseUnderscoreLabel) {
      addChunkWithTextNoCopy(
          CodeCompletionString::Chunk::ChunkKind::CallArgumentName, "_");
      addChunkWithTextNoCopy(
          CodeCompletionString::Chunk::ChunkKind::CallArgumentColon, ": ");
    } else if (!LocalName.empty()) {
      // Use local (non-API) parameter name if we have nothing else.
      llvm::SmallString<16> EscapedKeyword;
      addChunkWithText(
          CodeCompletionString::Chunk::ChunkKind::CallArgumentInternalName,
          escapeKeyword(LocalName.str(), false, EscapedKeyword));
      addChunkWithTextNoCopy(
          CodeCompletionString::Chunk::ChunkKind::CallArgumentColon, ": ");
    }
  }

  // 'inout' arguments are printed specially.
  if (IsInOut) {
    addChunkWithTextNoCopy(CodeCompletionString::Chunk::ChunkKind::Ampersand,
                           "&");
    Ty = Ty->getInOutObjectType();
  }

  // If the parameter is of the type @autoclosure ()->output, then the
  // code completion should show the parameter of the output type
  // instead of the function type ()->output.
  if (IsAutoClosure) {
    // 'Ty' may be ErrorType.
    if (auto funcTy = Ty->getAs<FunctionType>())
      Ty = funcTy->getResult();
  }

  PrintOptions PO;
  PO.SkipAttributes = true;
  PO.PrintOptionalAsImplicitlyUnwrapped = IsIUO;
  PO.OpaqueReturnTypePrinting =
      PrintOptions::OpaqueReturnTypePrintingMode::WithoutOpaqueKeyword;
  if (ContextTy)
    PO.setBaseType(ContextTy);
  if (shouldAnnotateResults()) {
    withNestedGroup(ChunkKind::CallArgumentTypeBegin, [&]() {
      CodeCompletionStringPrinter printer(*this);
      auto TL = TypeLoc::withoutLoc(Ty);
      printer.printTypePre(TL);
      Ty->print(printer, PO);
      printer.printTypePost(TL);
    });
  } else {
    std::string TypeName = Ty->getString(PO);
    addChunkWithText(ChunkKind::CallArgumentType, TypeName);
  }

  if (HasDefault) {
    withNestedGroup(ChunkKind::CallArgumentDefaultBegin, []() {
      // Possibly add the actual value in the future
    });
  }

  // Look through optional types and type aliases to find out if we have
  // function type.
  Ty = Ty->lookThroughAllOptionalTypes();
  if (auto AFT = Ty->getAs<AnyFunctionType>()) {
    // If this is a closure type, add ChunkKind::CallArgumentClosureType or
    // ChunkKind::CallArgumentClosureExpr for labeled trailing closures.
    PrintOptions PO;
    PO.PrintFunctionRepresentationAttrs =
        PrintOptions::FunctionRepresentationMode::None;
    PO.SkipAttributes = true;
    PO.OpaqueReturnTypePrinting =
        PrintOptions::OpaqueReturnTypePrintingMode::WithoutOpaqueKeyword;
    PO.AlwaysTryPrintParameterLabels = true;
    if (ContextTy)
      PO.setBaseType(ContextTy);

    if (IsLabeledTrailingClosure) {
      // Expand the closure body.
      SmallString<32> buffer;
      llvm::raw_svector_ostream OS(buffer);

      bool firstParam = true;
      for (const auto &param : AFT->getParams()) {
        if (!firstParam)
          OS << ", ";
        firstParam = false;

        if (param.hasLabel()) {
          OS << param.getLabel();
        } else if (param.hasInternalLabel()) {
          OS << param.getInternalLabel();
        } else {
          OS << "<#";
          if (param.isInOut())
            OS << "inout ";
          OS << param.getPlainType()->getString(PO);
          if (param.isVariadic())
            OS << "...";
          OS << "#>";
        }
      }

      if (!firstParam)
        OS << " in";

      addChunkWithText(
          CodeCompletionString::Chunk::ChunkKind::CallArgumentClosureExpr,
          OS.str());
    } else {
      // Add the closure type.
      addChunkWithText(
          CodeCompletionString::Chunk::ChunkKind::CallArgumentClosureType,
          AFT->getString(PO));
    }
  }

  if (IsVarArg)
    addEllipsis();

  --CurrentNestingLevel;
}

void CodeCompletionResultBuilder::withNestedGroup(
    CodeCompletionString::Chunk::ChunkKind Kind,
    llvm::function_ref<void()> body) {
  ++CurrentNestingLevel;
  addSimpleChunk(Kind);
  body();
  --CurrentNestingLevel;
}

void CodeCompletionResultBuilder::addTypeAnnotation(Type T, PrintOptions PO,
                                                    StringRef suffix) {
  T = T->getReferenceStorageReferent();

  // Replace '()' with 'Void'.
  if (T->isVoid())
    T = T->getASTContext().getVoidDecl()->getDeclaredInterfaceType();

  if (shouldAnnotateResults()) {
    withNestedGroup(CodeCompletionString::Chunk::ChunkKind::TypeAnnotationBegin,
                    [&]() {
                      CodeCompletionStringPrinter printer(*this);
                      auto TL = TypeLoc::withoutLoc(T);
                      printer.printTypePre(TL);
                      T->print(printer, PO);
                      printer.printTypePost(TL);
                      if (!suffix.empty())
                        printer.printText(suffix);
                    });
  } else {
    auto str = T.getString(PO);
    if (!suffix.empty())
      str += suffix.str();
    addTypeAnnotation(str);
  }
}
