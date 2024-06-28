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

#include "ExtractExprBase.h"
#include "RefactoringActions.h"
#include "Utils.h"
#include "swift/AST/DiagnosticsRefactoring.h"
#include "swift/Basic/Assertions.h"
#include "swift/IDETool/CompilerInvocation.h"

using namespace swift::refactoring;

static Type sanitizeType(Type Ty) {
  // Transform lvalue type to inout type so that we can print it properly.
  return Ty.transform([](Type Ty) {
    if (Ty->is<LValueType>()) {
      return Type(InOutType::get(Ty->getRValueType()->getCanonicalType()));
    }
    return Ty;
  });
}

static SourceLoc getNewFuncInsertLoc(DeclContext *DC,
                                     DeclContext *&InsertToContext) {
  if (auto D = DC->getInnermostDeclarationDeclContext()) {

    // If extracting from a getter/setter, we should skip both the immediate
    // getter/setter function and the individual var decl. The pattern binding
    // decl is the position before which we should insert the newly extracted
    // function.
    if (auto *FD = dyn_cast<AccessorDecl>(D)) {
      ValueDecl *SD = FD->getStorage();
      switch (SD->getKind()) {
      case DeclKind::Var:
        if (auto *PBD = cast<VarDecl>(SD)->getParentPatternBinding())
          D = PBD;
        break;
      case DeclKind::Subscript:
        D = SD;
        break;
      default:
        break;
      }
    }

    auto Result = D->getStartLoc();
    assert(Result.isValid());

    // The insert loc should be before every decl attributes.
    for (auto Attr : D->getAttrs()) {
      auto Loc = Attr->getRangeWithAt().Start;
      if (Loc.isValid() &&
          Loc.getOpaquePointerValue() < Result.getOpaquePointerValue())
        Result = Loc;
    }

    // The insert loc should be before the doc comments associated with this
    // decl.
    if (!D->getRawComment().Comments.empty()) {
      auto Loc = D->getRawComment().Comments.front().Range.getStart();
      if (Loc.isValid() &&
          Loc.getOpaquePointerValue() < Result.getOpaquePointerValue()) {
        Result = Loc;
      }
    }
    InsertToContext = D->getDeclContext();
    return Result;
  }
  return SourceLoc();
}

#if SWIFT_BUILD_SWIFT_SYNTAX
static std::vector<NoteRegion>
getNotableRegions(StringRef SourceText, unsigned NameOffset, StringRef Name) {
  auto InputBuffer =
      llvm::MemoryBuffer::getMemBufferCopy(SourceText, "<extract>");

  CompilerInvocation Invocation{};

  Invocation.getFrontendOptions().InputsAndOutputs.addInput(
      InputFile("<extract>", true, InputBuffer.get(), file_types::TY_Swift));
  Invocation.getFrontendOptions().ModuleName = "extract";
  Invocation.getLangOptions().DisablePoundIfEvaluation = true;

  auto Instance = std::make_unique<swift::CompilerInstance>();
  std::string InstanceSetupError;
  if (Instance->setup(Invocation, InstanceSetupError))
    llvm_unreachable(InstanceSetupError.c_str());

  unsigned BufferId = Instance->getPrimarySourceFile()->getBufferID().value();
  SourceManager &SM = Instance->getSourceMgr();
  SourceLoc NameLoc = SM.getLocForOffset(BufferId, NameOffset);
  auto LineAndCol = SM.getLineAndColumnInBuffer(NameLoc);

  auto Resolved = runNameMatcher(*Instance->getPrimarySourceFile(), NameLoc);
  assert(!Resolved.empty() && "Failed to resolve generated func name loc");

  RenameLoc RenameConfig = {LineAndCol.first, LineAndCol.second,
                            RenameLocUsage::Definition, /*OldName=*/Name};
  std::vector<RenameRangeDetail> Ranges =
      getSyntacticRenameRangeDetails(SM, Name, Resolved.back(), RenameConfig)
          .Ranges;

  std::vector<NoteRegion> NoteRegions(Ranges.size());
  llvm::transform(Ranges, NoteRegions.begin(),
                  [&SM](RenameRangeDetail &Detail) -> NoteRegion {
                    auto Start =
                        SM.getLineAndColumnInBuffer(Detail.Range.getStart());
                    auto End =
                        SM.getLineAndColumnInBuffer(Detail.Range.getEnd());
                    return {Detail.RangeKind, Start.first, Start.second,
                            End.first,        End.second,  Detail.Index};
                  });

  return NoteRegions;
}
#endif // SWIFT_BUILD_SWIFT_SYNTAX

bool RefactoringActionExtractFunction::isApplicable(
    const ResolvedRangeInfo &Info, DiagnosticEngine &Diag) {
  switch (Info.Kind) {
  case RangeKind::PartOfExpression:
  case RangeKind::SingleDecl:
  case RangeKind::MultiTypeMemberDecl:
  case RangeKind::Invalid:
    return false;
  case RangeKind::SingleExpression:
  case RangeKind::SingleStatement:
  case RangeKind::MultiStatement: {
    return checkExtractConditions(Info, Diag)
        .success({CannotExtractReason::VoidType});
  }
  }
  llvm_unreachable("unhandled kind");
}

bool RefactoringActionExtractFunction::performChange() {
#if !SWIFT_BUILD_SWIFT_SYNTAX
  DiagEngine.diagnose(SourceLoc(),
                      diag::extract_function_not_supported_swiftsyntax_missing);
  return true;
#else
  // Check if the new name is ok.
  if (!Lexer::isIdentifier(PreferredName)) {
    DiagEngine.diagnose(SourceLoc(), diag::invalid_name, PreferredName);
    return true;
  }
  DeclContext *DC = RangeInfo.RangeContext;
  DeclContext *InsertToDC = nullptr;
  SourceLoc InsertLoc = getNewFuncInsertLoc(DC, InsertToDC);

  // Complain about no inserting position.
  if (InsertLoc.isInvalid()) {
    DiagEngine.diagnose(SourceLoc(), diag::no_insert_position);
    return true;
  }

  // Correct the given name if collision happens.
  PreferredName = correctNewDeclName(InsertLoc, InsertToDC, PreferredName);

  // Collect the parameters to pass down to the new function.
  std::vector<ReferencedDecl> Parameters;
  for (auto &RD : RangeInfo.ReferencedDecls) {
    // If the referenced decl is declared elsewhere, no need to pass as
    // parameter
    if (RD.VD->getDeclContext() != DC)
      continue;

    // We don't need to pass down implicitly declared variables, e.g. error in
    // a catch block.
    if (RD.VD->isImplicit()) {
      SourceLoc Loc = RD.VD->getStartLoc();
      if (Loc.isValid() &&
          SM.isBeforeInBuffer(RangeInfo.ContentRange.getStart(), Loc) &&
          SM.isBeforeInBuffer(Loc, RangeInfo.ContentRange.getEnd()))
        continue;
    }

    // If the referenced decl is declared inside the range, no need to pass
    // as parameter.
    if (RangeInfo.DeclaredDecls.end() !=
        std::find_if(RangeInfo.DeclaredDecls.begin(),
                     RangeInfo.DeclaredDecls.end(),
                     [RD](DeclaredDecl DD) { return RD.VD == DD.VD; }))
      continue;

    // We don't need to pass down self.
    if (auto PD = dyn_cast<ParamDecl>(RD.VD)) {
      if (PD->isSelfParameter()) {
        continue;
      }
    }

    Parameters.emplace_back(RD.VD, sanitizeType(RD.Ty));
  }
  SmallString<64> Buffer;
  unsigned FuncBegin = Buffer.size();
  unsigned FuncNameOffset;
  {
    llvm::raw_svector_ostream OS(Buffer);

    if (!InsertToDC->isLocalContext()) {
      // Default to be file private.
      OS << tok::kw_fileprivate << " ";
    }

    // Inherit static if the containing function is.
    if (DC->getContextKind() == DeclContextKind::AbstractFunctionDecl) {
      if (auto FD =
              dyn_cast<FuncDecl>(static_cast<AbstractFunctionDecl *>(DC))) {
        if (FD->isStatic()) {
          OS << tok::kw_static << " ";
        }
      }
    }

    OS << tok::kw_func << " ";
    FuncNameOffset = Buffer.size() - FuncBegin;
    OS << PreferredName;
    OS << "(";
    for (auto &RD : Parameters) {
      OS << "_ " << RD.VD->getBaseName().userFacingName() << ": ";
      RD.Ty->reconstituteSugar(/*Recursive*/ true)->print(OS);
      if (&RD != &Parameters.back())
        OS << ", ";
    }
    OS << ")";

    if (RangeInfo.UnhandledEffects.contains(EffectKind::Async))
      OS << " async";
    if (RangeInfo.UnhandledEffects.contains(EffectKind::Throws))
      OS << " " << tok::kw_throws;

    bool InsertedReturnType = false;
    if (auto Ty = RangeInfo.getType()) {
      // If the type of the range is not void, specify the return type.
      if (!Ty->isVoid()) {
        OS << " " << tok::arrow << " ";
        sanitizeType(Ty)->reconstituteSugar(/*Recursive*/ true)->print(OS);
        InsertedReturnType = true;
      }
    }

    OS << " {\n";

    // Add "return" if the extracted entity is an expression.
    if (RangeInfo.Kind == RangeKind::SingleExpression && InsertedReturnType)
      OS << tok::kw_return << " ";
    OS << RangeInfo.ContentRange.str() << "\n}\n\n";
  }
  unsigned FuncEnd = Buffer.size();

  unsigned ReplaceBegin = Buffer.size();
  unsigned CallNameOffset;
  {
    llvm::raw_svector_ostream OS(Buffer);
    if (RangeInfo.exit() == ExitState::Positive)
      OS << tok::kw_return << " ";

    if (RangeInfo.UnhandledEffects.contains(EffectKind::Throws))
      OS << tok::kw_try << " ";
    if (RangeInfo.UnhandledEffects.contains(EffectKind::Async))
      OS << "await ";

    CallNameOffset = Buffer.size() - ReplaceBegin;
    OS << PreferredName << "(";
    for (auto &RD : Parameters) {

      // Inout argument needs "&".
      if (RD.Ty->is<InOutType>())
        OS << "&";
      OS << RD.VD->getBaseName().userFacingName();
      if (&RD != &Parameters.back())
        OS << ", ";
    }
    OS << ")";
  }
  unsigned ReplaceEnd = Buffer.size();

  std::string ExtractedFuncName = PreferredName.str() + "(";
  for (size_t i = 0; i < Parameters.size(); ++i) {
    ExtractedFuncName += "_:";
  }
  ExtractedFuncName += ")";

  StringRef DeclStr(Buffer.begin() + FuncBegin, FuncEnd - FuncBegin);
  auto NotableFuncRegions =
      getNotableRegions(DeclStr, FuncNameOffset, ExtractedFuncName);

  StringRef CallStr(Buffer.begin() + ReplaceBegin, ReplaceEnd - ReplaceBegin);
  auto NotableCallRegions =
      getNotableRegions(CallStr, CallNameOffset, ExtractedFuncName);

  // Insert the new function's declaration.
  EditConsumer.accept(SM, InsertLoc, DeclStr, NotableFuncRegions);

  // Replace the code to extract with the function call.
  EditConsumer.accept(SM, RangeInfo.ContentRange, CallStr, NotableCallRegions);

  return false;
#endif
}
