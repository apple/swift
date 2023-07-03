//===--- CodeCompletionResult.cpp -----------------------------------------===//
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

#include "swift/IDE/CodeCompletionResult.h"
#include "CodeCompletionDiagnostics.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Module.h"
#include "swift/IDE/CodeCompletionResultPrinter.h"
#include "swift/IDE/CodeCompletionResultSink.h"

using namespace swift;
using namespace swift::ide;

CodeCompletionMacroRoles swift::ide::getCompletionMacroRoles(const Decl *D) {
  CodeCompletionMacroRoles roles;

  auto *MD = dyn_cast<MacroDecl>(D);
  if (!MD)
    return roles;

  MacroRoles macroRoles = MD->getMacroRoles();
  if (macroRoles.contains(MacroRole::Expression)) {
    roles |= CodeCompletionMacroRole::Expression;
  }
  if (macroRoles.contains(MacroRole::Declaration)) {
    roles |= CodeCompletionMacroRole::Declaration;
  }
  if (macroRoles.contains(MacroRole::CodeItem)) {
    roles |= CodeCompletionMacroRole::CodeItem;
  }
  if (macroRoles.contains(MacroRole::Accessor)) {
    roles |= CodeCompletionMacroRole::AttachedVar;
  }
  if (macroRoles & MacroRoles({MacroRole::MemberAttribute, MacroRole::Member,
                               MacroRole::Conformance,
                               MacroRole::Extension,})) {
    roles |= CodeCompletionMacroRole::AttachedContext;
  }
  if (macroRoles.contains(MacroRole::Peer)) {
    roles |= CodeCompletionMacroRole::AttachedDecl;
  }

  return roles;
}

CodeCompletionMacroRoles
swift::ide::getCompletionMacroRoles(OptionSet<CustomAttributeKind> kinds) {
  CodeCompletionMacroRoles roles;
  if (kinds.contains(CustomAttributeKind::VarMacro)) {
    roles |= CodeCompletionMacroRole::AttachedVar;
  }
  if (kinds.contains(CustomAttributeKind::ContextMacro)) {
    roles |= CodeCompletionMacroRole::AttachedContext;
  }
  if (kinds.contains(CustomAttributeKind::DeclMacro)) {
    roles |= CodeCompletionMacroRole::AttachedDecl;
  }
  return roles;
}

CodeCompletionMacroRoles
swift::ide::getCompletionMacroRoles(CodeCompletionFilter filter) {
  CodeCompletionMacroRoles roles;
  if (filter.contains(CodeCompletionFilterFlag::ExpressionMacro)) {
    roles |= CodeCompletionMacroRole::Expression;
  }
  if (filter.contains(CodeCompletionFilterFlag::DeclarationMacro)) {
    roles |= CodeCompletionMacroRole::Declaration;
  }
  if (filter.contains(CodeCompletionFilterFlag::CodeItemMacro)) {
    roles |= CodeCompletionMacroRole::CodeItem;
  }
  if (filter.contains(CodeCompletionFilterFlag::AttachedVarMacro)) {
    roles |= CodeCompletionMacroRole::AttachedVar;
  }
  if (filter.contains(CodeCompletionFilterFlag::AttachedContextMacro)) {
    roles |= CodeCompletionMacroRole::AttachedContext;
  }
  if (filter.contains(CodeCompletionFilterFlag::AttachedDeclMacro)) {
    roles |= CodeCompletionMacroRole::AttachedDecl;
  }
  return roles;
}

CodeCompletionFilter
swift::ide::getCompletionFilter(CodeCompletionMacroRoles roles) {
  CodeCompletionFilter filter;
  if (roles.contains(CodeCompletionMacroRole::Expression)) {
    filter |= CodeCompletionFilterFlag::ExpressionMacro;
  }
  if (roles.contains(CodeCompletionMacroRole::Declaration)) {
    filter |= CodeCompletionFilterFlag::DeclarationMacro;
  }
  if (roles.contains(CodeCompletionMacroRole::CodeItem)) {
    filter |= CodeCompletionFilterFlag::CodeItemMacro;
  }
  if (roles.contains(CodeCompletionMacroRole::AttachedVar)) {
    filter |= CodeCompletionFilterFlag::AttachedVarMacro;
  }
  if (roles.contains(CodeCompletionMacroRole::AttachedContext)) {
    filter |= CodeCompletionFilterFlag::AttachedContextMacro;
  }
  if (roles.contains(CodeCompletionMacroRole::AttachedDecl)) {
    filter |= CodeCompletionFilterFlag::AttachedDeclMacro;
  }
  return filter;
}

// MARK: - ContextFreeCodeCompletionResult

ContextFreeCodeCompletionResult *
ContextFreeCodeCompletionResult::createPatternOrBuiltInOperatorResult(
    CodeCompletionResultSink &Sink, CodeCompletionResultKind Kind,
    CodeCompletionString *CompletionString,
    CodeCompletionOperatorKind KnownOperatorKind, bool IsAsync,
    NullTerminatedStringRef BriefDocComment,
    CodeCompletionResultType ResultType,
    ContextFreeNotRecommendedReason NotRecommended,
    CodeCompletionDiagnosticSeverity DiagnosticSeverity,
    NullTerminatedStringRef DiagnosticMessage) {
  if (Sink.shouldProduceContextFreeResults()) {
    ResultType = ResultType.usrBasedType(Sink.getUSRTypeArena());
  }
  NullTerminatedStringRef NameForDiagnostics;
  if (KnownOperatorKind == CodeCompletionOperatorKind::None) {
    NameForDiagnostics = "function";
  } else {
    NameForDiagnostics = "operator";
  }
  return new (Sink.getAllocator()) ContextFreeCodeCompletionResult(
      Kind, /*AssociatedKind=*/0, KnownOperatorKind, /*MacroRoles=*/{},
      /*IsSystem=*/false, IsAsync, /*HasAsyncAlternative=*/false,
      CompletionString,
      /*ModuleName=*/"", BriefDocComment,
      /*AssociatedUSRs=*/{}, ResultType, NotRecommended, DiagnosticSeverity,
      DiagnosticMessage,
      getCodeCompletionResultFilterName(CompletionString, Sink.getAllocator()),
      NameForDiagnostics);
}

ContextFreeCodeCompletionResult *
ContextFreeCodeCompletionResult::createKeywordResult(
    CodeCompletionResultSink &Sink, CodeCompletionKeywordKind Kind,
    CodeCompletionString *CompletionString,
    NullTerminatedStringRef BriefDocComment,
    CodeCompletionResultType ResultType) {
  if (Sink.shouldProduceContextFreeResults()) {
    ResultType = ResultType.usrBasedType(Sink.getUSRTypeArena());
  }
  return new (Sink.getAllocator()) ContextFreeCodeCompletionResult(
      CodeCompletionResultKind::Keyword, static_cast<uint8_t>(Kind),
      CodeCompletionOperatorKind::None, /*MacroRoles=*/{},
      /*IsSystem=*/false, /*IsAsync=*/false,
      /*HasAsyncAlternative=*/false, CompletionString,
      /*ModuleName=*/"", BriefDocComment,
      /*AssociatedUSRs=*/{}, ResultType, ContextFreeNotRecommendedReason::None,
      CodeCompletionDiagnosticSeverity::None, /*DiagnosticMessage=*/"",
      getCodeCompletionResultFilterName(CompletionString, Sink.getAllocator()),
      /*NameForDiagnostics=*/"");
}

ContextFreeCodeCompletionResult *
ContextFreeCodeCompletionResult::createLiteralResult(
    CodeCompletionResultSink &Sink, CodeCompletionLiteralKind LiteralKind,
    CodeCompletionString *CompletionString,
    CodeCompletionResultType ResultType) {
  if (Sink.shouldProduceContextFreeResults()) {
    ResultType = ResultType.usrBasedType(Sink.getUSRTypeArena());
  }
  return new (Sink.getAllocator()) ContextFreeCodeCompletionResult(
      CodeCompletionResultKind::Literal, static_cast<uint8_t>(LiteralKind),
      CodeCompletionOperatorKind::None, /*MacroRoles=*/{},
      /*IsSystem=*/false, /*IsAsync=*/false, /*HasAsyncAlternative=*/false,
      CompletionString,
      /*ModuleName=*/"",
      /*BriefDocComment=*/"",
      /*AssociatedUSRs=*/{}, ResultType, ContextFreeNotRecommendedReason::None,
      CodeCompletionDiagnosticSeverity::None, /*DiagnosticMessage=*/"",
      getCodeCompletionResultFilterName(CompletionString, Sink.getAllocator()),
      /*NameForDiagnostics=*/"");
}

static NullTerminatedStringRef
getDeclNameForDiagnostics(const Decl *D, CodeCompletionResultSink &Sink) {
  if (auto VD = dyn_cast<ValueDecl>(D)) {
    llvm::SmallString<64> Name;
    llvm::raw_svector_ostream NameOS(Name);
    NameOS << "'";
    llvm::SmallString<64> Scratch;
    VD->getName().printPretty(NameOS);
    NameOS << "'";
    return NullTerminatedStringRef(NameOS.str(), Sink.getAllocator());
  } else {
    return "";
  }
}

ContextFreeCodeCompletionResult *
ContextFreeCodeCompletionResult::createDeclResult(
    CodeCompletionResultSink &Sink, CodeCompletionString *CompletionString,
    const Decl *AssociatedDecl, bool IsAsync, bool HasAsyncAlternative,
    NullTerminatedStringRef ModuleName, NullTerminatedStringRef BriefDocComment,
    ArrayRef<NullTerminatedStringRef> AssociatedUSRs,
    CodeCompletionResultType ResultType,
    ContextFreeNotRecommendedReason NotRecommended,
    CodeCompletionDiagnosticSeverity DiagnosticSeverity,
    NullTerminatedStringRef DiagnosticMessage) {
  assert(AssociatedDecl && "should have a decl");
  if (Sink.shouldProduceContextFreeResults()) {
    ResultType = ResultType.usrBasedType(Sink.getUSRTypeArena());
  }
  return new (Sink.getAllocator()) ContextFreeCodeCompletionResult(
      CodeCompletionResultKind::Declaration,
      static_cast<uint8_t>(getCodeCompletionDeclKind(AssociatedDecl)),
      CodeCompletionOperatorKind::None, getCompletionMacroRoles(AssociatedDecl),
      getDeclIsSystem(AssociatedDecl), IsAsync, HasAsyncAlternative,
      CompletionString, ModuleName, BriefDocComment, AssociatedUSRs, ResultType,
      NotRecommended, DiagnosticSeverity, DiagnosticMessage,
      getCodeCompletionResultFilterName(CompletionString, Sink.getAllocator()),
      /*NameForDiagnostics=*/getDeclNameForDiagnostics(AssociatedDecl, Sink));
}

CodeCompletionOperatorKind
ContextFreeCodeCompletionResult::getCodeCompletionOperatorKind(
    const CodeCompletionString *str) {
  StringRef name = str->getFirstTextChunk(/*includeLeadingPunctuation=*/true);
  using CCOK = CodeCompletionOperatorKind;
  using OpPair = std::pair<StringRef, CCOK>;

  // This list must be kept in alphabetical order.
  static OpPair ops[] = {
      std::make_pair("!", CCOK::Bang),
      std::make_pair("!=", CCOK::NotEq),
      std::make_pair("!==", CCOK::NotEqEq),
      std::make_pair("%", CCOK::Modulo),
      std::make_pair("%=", CCOK::ModuloEq),
      std::make_pair("&", CCOK::Amp),
      std::make_pair("&&", CCOK::AmpAmp),
      std::make_pair("&*", CCOK::AmpStar),
      std::make_pair("&+", CCOK::AmpPlus),
      std::make_pair("&-", CCOK::AmpMinus),
      std::make_pair("&=", CCOK::AmpEq),
      std::make_pair("(", CCOK::LParen),
      std::make_pair("*", CCOK::Star),
      std::make_pair("*=", CCOK::StarEq),
      std::make_pair("+", CCOK::Plus),
      std::make_pair("+=", CCOK::PlusEq),
      std::make_pair("-", CCOK::Minus),
      std::make_pair("-=", CCOK::MinusEq),
      std::make_pair(".", CCOK::Dot),
      std::make_pair("...", CCOK::DotDotDot),
      std::make_pair("..<", CCOK::DotDotLess),
      std::make_pair("/", CCOK::Slash),
      std::make_pair("/=", CCOK::SlashEq),
      std::make_pair("<", CCOK::Less),
      std::make_pair("<<", CCOK::LessLess),
      std::make_pair("<<=", CCOK::LessLessEq),
      std::make_pair("<=", CCOK::LessEq),
      std::make_pair("=", CCOK::Eq),
      std::make_pair("==", CCOK::EqEq),
      std::make_pair("===", CCOK::EqEqEq),
      std::make_pair(">", CCOK::Greater),
      std::make_pair(">=", CCOK::GreaterEq),
      std::make_pair(">>", CCOK::GreaterGreater),
      std::make_pair(">>=", CCOK::GreaterGreaterEq),
      std::make_pair("?.", CCOK::QuestionDot),
      std::make_pair("^", CCOK::Caret),
      std::make_pair("^=", CCOK::CaretEq),
      std::make_pair("|", CCOK::Pipe),
      std::make_pair("|=", CCOK::PipeEq),
      std::make_pair("||", CCOK::PipePipe),
      std::make_pair("~=", CCOK::TildeEq),
  };
  static auto opsSize = sizeof(ops) / sizeof(ops[0]);

  auto I = std::lower_bound(
      ops, &ops[opsSize], std::make_pair(name, CCOK::None),
      [](const OpPair &a, const OpPair &b) { return a.first < b.first; });

  if (I == &ops[opsSize] || I->first != name)
    return CCOK::Unknown;
  return I->second;
}

CodeCompletionDeclKind
ContextFreeCodeCompletionResult::getCodeCompletionDeclKind(const Decl *D) {
  switch (D->getKind()) {
  case DeclKind::Import:
  case DeclKind::Extension:
  case DeclKind::PatternBinding:
  case DeclKind::EnumCase:
  case DeclKind::TopLevelCode:
  case DeclKind::IfConfig:
  case DeclKind::PoundDiagnostic:
  case DeclKind::Missing:
  case DeclKind::MissingMember:
  case DeclKind::OpaqueType:
  case DeclKind::BuiltinTuple:
  case DeclKind::MacroExpansion:
    llvm_unreachable("not expecting such a declaration result");
  case DeclKind::Module:
    return CodeCompletionDeclKind::Module;
  case DeclKind::TypeAlias:
    return CodeCompletionDeclKind::TypeAlias;
  case DeclKind::AssociatedType:
    return CodeCompletionDeclKind::AssociatedType;
  case DeclKind::GenericTypeParam:
    return CodeCompletionDeclKind::GenericTypeParam;
  case DeclKind::Enum:
    return CodeCompletionDeclKind::Enum;
  case DeclKind::Struct:
    return CodeCompletionDeclKind::Struct;
  case DeclKind::Class:
    if (cast<ClassDecl>(D)->isAnyActor()) {
      return CodeCompletionDeclKind::Actor;
    } else {
      return CodeCompletionDeclKind::Class;
    }
  case DeclKind::Protocol:
    return CodeCompletionDeclKind::Protocol;
  case DeclKind::Var:
  case DeclKind::Param: {
    auto DC = D->getDeclContext();
    if (DC->isTypeContext()) {
      if (cast<VarDecl>(D)->isStatic())
        return CodeCompletionDeclKind::StaticVar;
      else
        return CodeCompletionDeclKind::InstanceVar;
    }
    if (DC->isLocalContext())
      return CodeCompletionDeclKind::LocalVar;
    return CodeCompletionDeclKind::GlobalVar;
  }
  case DeclKind::Constructor:
    return CodeCompletionDeclKind::Constructor;
  case DeclKind::Destructor:
    return CodeCompletionDeclKind::Destructor;
  case DeclKind::Accessor:
  case DeclKind::Func: {
    auto DC = D->getDeclContext();
    auto FD = cast<FuncDecl>(D);
    if (DC->isTypeContext()) {
      if (FD->isStatic())
        return CodeCompletionDeclKind::StaticMethod;
      return CodeCompletionDeclKind::InstanceMethod;
    }
    if (FD->isOperator()) {
      if (auto op = FD->getOperatorDecl()) {
        switch (op->getKind()) {
        case DeclKind::PrefixOperator:
          return CodeCompletionDeclKind::PrefixOperatorFunction;
        case DeclKind::PostfixOperator:
          return CodeCompletionDeclKind::PostfixOperatorFunction;
        case DeclKind::InfixOperator:
          return CodeCompletionDeclKind::InfixOperatorFunction;
        default:
          llvm_unreachable("unexpected operator kind");
        }
      } else {
        return CodeCompletionDeclKind::InfixOperatorFunction;
      }
    }
    return CodeCompletionDeclKind::FreeFunction;
  }
  case DeclKind::InfixOperator:
    return CodeCompletionDeclKind::InfixOperatorFunction;
  case DeclKind::PrefixOperator:
    return CodeCompletionDeclKind::PrefixOperatorFunction;
  case DeclKind::PostfixOperator:
    return CodeCompletionDeclKind::PostfixOperatorFunction;
  case DeclKind::PrecedenceGroup:
    return CodeCompletionDeclKind::PrecedenceGroup;
  case DeclKind::EnumElement:
    return CodeCompletionDeclKind::EnumElement;
  case DeclKind::Subscript:
    return CodeCompletionDeclKind::Subscript;
  case DeclKind::Macro:
    return CodeCompletionDeclKind::Macro;
  }
  llvm_unreachable("invalid DeclKind");
}

bool ContextFreeCodeCompletionResult::getDeclIsSystem(const Decl *D) {
  return D->getModuleContext()->isNonUserModule();
}

ContextualNotRecommendedReason
ContextFreeCodeCompletionResult::calculateContextualNotRecommendedReason(
    ContextualNotRecommendedReason explicitReason,
    bool canCurrDeclContextHandleAsync) const {
  if (explicitReason != ContextualNotRecommendedReason::None) {
    return explicitReason;
  }
  if (IsAsync && !canCurrDeclContextHandleAsync) {
    return ContextualNotRecommendedReason::InvalidAsyncContext;
  }
  if (HasAsyncAlternative && canCurrDeclContextHandleAsync) {
    return ContextualNotRecommendedReason::
        NonAsyncAlternativeUsedInAsyncContext;
  }
  return ContextualNotRecommendedReason::None;
}

CodeCompletionResultTypeRelation
ContextFreeCodeCompletionResult::calculateContextualTypeRelation(
    const DeclContext *dc, const ExpectedTypeContext *typeContext,
    const USRBasedTypeContext *usrTypeContext) const {
  CodeCompletionResultTypeRelation typeRelation =
      getResultType().calculateTypeRelation(typeContext, dc, usrTypeContext);
  if (typeRelation >= CodeCompletionResultTypeRelation::Convertible ||
      !typeContext)
    return typeRelation;

  CodeCompletionMacroRoles expectedRoles =
      getCompletionMacroRoles(typeContext->getExpectedCustomAttributeKinds());
  if (MacroRoles & expectedRoles)
    return CodeCompletionResultTypeRelation::Convertible;
  return typeRelation;
}

// MARK: - CodeCompletionResult

CodeCompletionResult *
CodeCompletionResult::withFlair(CodeCompletionFlair NewFlair,
                                CodeCompletionResultSink &Sink) const {
  return new (*Sink.Allocator)
      CodeCompletionResult(ContextFree, SemanticContext, NewFlair,
                           NumBytesToErase, TypeDistance, NotRecommended);
}

CodeCompletionResult *
CodeCompletionResult::withContextFreeResultSemanticContextAndFlair(
    const ContextFreeCodeCompletionResult &NewContextFree,
    SemanticContextKind NewSemanticContext, CodeCompletionFlair NewFlair,
    CodeCompletionResultSink &Sink) const {
  return new (*Sink.Allocator)
      CodeCompletionResult(NewContextFree, NewSemanticContext, NewFlair,
                           NumBytesToErase, TypeDistance, NotRecommended);
}

std::pair<CodeCompletionDiagnosticSeverity, NullTerminatedStringRef>
CodeCompletionResult::getContextualDiagnosticSeverityAndMessage(
    SmallVectorImpl<char> &Scratch, const ASTContext &Ctx) const {
  llvm::raw_svector_ostream Out(Scratch);
  CodeCompletionDiagnosticSeverity Severity;
  getContextualCompletionDiagnostics(
      NotRecommended, ContextFree.getNameForDiagnostics(), Severity, Out, Ctx);
  Out << '\0';
  NullTerminatedStringRef Message(Out.str().data(), Out.str().size() - 1);
  return std::make_pair(Severity, Message);
}

void CodeCompletionResult::printPrefix(raw_ostream &OS) const {
  llvm::SmallString<64> Prefix;
  switch (getKind()) {
  case CodeCompletionResultKind::Declaration:
    Prefix.append("Decl");
    switch (getAssociatedDeclKind()) {
    case CodeCompletionDeclKind::Class:
      Prefix.append("[Class]");
      break;
    case CodeCompletionDeclKind::Actor:
      Prefix.append("[Actor]");
      break;
    case CodeCompletionDeclKind::Struct:
      Prefix.append("[Struct]");
      break;
    case CodeCompletionDeclKind::Enum:
      Prefix.append("[Enum]");
      break;
    case CodeCompletionDeclKind::EnumElement:
      Prefix.append("[EnumElement]");
      break;
    case CodeCompletionDeclKind::Protocol:
      Prefix.append("[Protocol]");
      break;
    case CodeCompletionDeclKind::TypeAlias:
      Prefix.append("[TypeAlias]");
      break;
    case CodeCompletionDeclKind::AssociatedType:
      Prefix.append("[AssociatedType]");
      break;
    case CodeCompletionDeclKind::GenericTypeParam:
      Prefix.append("[GenericTypeParam]");
      break;
    case CodeCompletionDeclKind::Constructor:
      Prefix.append("[Constructor]");
      break;
    case CodeCompletionDeclKind::Destructor:
      Prefix.append("[Destructor]");
      break;
    case CodeCompletionDeclKind::Subscript:
      Prefix.append("[Subscript]");
      break;
    case CodeCompletionDeclKind::StaticMethod:
      Prefix.append("[StaticMethod]");
      break;
    case CodeCompletionDeclKind::InstanceMethod:
      Prefix.append("[InstanceMethod]");
      break;
    case CodeCompletionDeclKind::PrefixOperatorFunction:
      Prefix.append("[PrefixOperatorFunction]");
      break;
    case CodeCompletionDeclKind::PostfixOperatorFunction:
      Prefix.append("[PostfixOperatorFunction]");
      break;
    case CodeCompletionDeclKind::InfixOperatorFunction:
      Prefix.append("[InfixOperatorFunction]");
      break;
    case CodeCompletionDeclKind::FreeFunction:
      Prefix.append("[FreeFunction]");
      break;
    case CodeCompletionDeclKind::StaticVar:
      Prefix.append("[StaticVar]");
      break;
    case CodeCompletionDeclKind::InstanceVar:
      Prefix.append("[InstanceVar]");
      break;
    case CodeCompletionDeclKind::LocalVar:
      Prefix.append("[LocalVar]");
      break;
    case CodeCompletionDeclKind::GlobalVar:
      Prefix.append("[GlobalVar]");
      break;
    case CodeCompletionDeclKind::Module:
      Prefix.append("[Module]");
      break;
    case CodeCompletionDeclKind::PrecedenceGroup:
      Prefix.append("[PrecedenceGroup]");
      break;
    case CodeCompletionDeclKind::Macro:
      Prefix.append("[Macro]");
      break;
    }
    break;
  case CodeCompletionResultKind::Keyword:
    Prefix.append("Keyword");
    switch (getKeywordKind()) {
    case CodeCompletionKeywordKind::None:
      break;
#define KEYWORD(X)                                                             \
  case CodeCompletionKeywordKind::kw_##X:                                      \
    Prefix.append("[" #X "]");                                                 \
    break;
#define POUND_KEYWORD(X)                                                       \
  case CodeCompletionKeywordKind::pound_##X:                                   \
    Prefix.append("[#" #X "]");                                                \
    break;
#include "swift/AST/TokenKinds.def"
    }
    break;
  case CodeCompletionResultKind::Pattern:
    Prefix.append("Pattern");
    break;
  case CodeCompletionResultKind::Literal:
    Prefix.append("Literal");
    switch (getLiteralKind()) {
    case CodeCompletionLiteralKind::ArrayLiteral:
      Prefix.append("[Array]");
      break;
    case CodeCompletionLiteralKind::BooleanLiteral:
      Prefix.append("[Boolean]");
      break;
    case CodeCompletionLiteralKind::ColorLiteral:
      Prefix.append("[_Color]");
      break;
    case CodeCompletionLiteralKind::ImageLiteral:
      Prefix.append("[_Image]");
      break;
    case CodeCompletionLiteralKind::DictionaryLiteral:
      Prefix.append("[Dictionary]");
      break;
    case CodeCompletionLiteralKind::IntegerLiteral:
      Prefix.append("[Integer]");
      break;
    case CodeCompletionLiteralKind::NilLiteral:
      Prefix.append("[Nil]");
      break;
    case CodeCompletionLiteralKind::StringLiteral:
      Prefix.append("[String]");
      break;
    case CodeCompletionLiteralKind::Tuple:
      Prefix.append("[Tuple]");
      break;
    }
    break;
  case CodeCompletionResultKind::BuiltinOperator:
    Prefix.append("BuiltinOperator");
    break;
  }
  Prefix.append("/");
  switch (getSemanticContext()) {
  case SemanticContextKind::None:
    Prefix.append("None");
    break;
  case SemanticContextKind::Local:
    Prefix.append("Local");
    break;
  case SemanticContextKind::CurrentNominal:
    Prefix.append("CurrNominal");
    break;
  case SemanticContextKind::Super:
    Prefix.append("Super");
    break;
  case SemanticContextKind::OutsideNominal:
    Prefix.append("OutNominal");
    break;
  case SemanticContextKind::CurrentModule:
    Prefix.append("CurrModule");
    break;
  case SemanticContextKind::OtherModule:
    Prefix.append("OtherModule");
    if (!getModuleName().empty())
      Prefix.append((Twine("[") + StringRef(getModuleName()) + "]").str());
    break;
  }
  if (getFlair().toRaw()) {
    Prefix.append("/Flair[");
    bool isFirstFlair = true;
#define PRINT_FLAIR(KIND, NAME)                                                \
  if (getFlair().contains(CodeCompletionFlairBit::KIND)) {                     \
    if (isFirstFlair) {                                                        \
      isFirstFlair = false;                                                    \
    } else {                                                                   \
      Prefix.append(",");                                                      \
    }                                                                          \
    Prefix.append(NAME);                                                       \
  }
    PRINT_FLAIR(ExpressionSpecific, "ExprSpecific");
    PRINT_FLAIR(SuperChain, "SuperChain");
    PRINT_FLAIR(ArgumentLabels, "ArgLabels");
    PRINT_FLAIR(CommonKeywordAtCurrentPosition, "CommonKeyword")
    PRINT_FLAIR(RareKeywordAtCurrentPosition, "RareKeyword")
    PRINT_FLAIR(RareTypeAtCurrentPosition, "RareType")
    PRINT_FLAIR(ExpressionAtNonScriptOrMainFileScope, "ExprAtFileScope")
    Prefix.append("]");
  }
  if (isNotRecommended())
    Prefix.append("/NotRecommended");
  if (isSystem())
    Prefix.append("/IsSystem");
  if (NumBytesToErase != 0) {
    Prefix.append("/Erase[");
    Prefix.append(Twine(NumBytesToErase).str());
    Prefix.append("]");
  }
  switch (getExpectedTypeRelation()) {
  case CodeCompletionResultTypeRelation::Invalid:
    Prefix.append("/TypeRelation[Invalid]");
    break;
  case CodeCompletionResultTypeRelation::Convertible:
    Prefix.append("/TypeRelation[Convertible]");
    break;
  case CodeCompletionResultTypeRelation::NotApplicable:
  case CodeCompletionResultTypeRelation::Unknown:
  case CodeCompletionResultTypeRelation::Unrelated:
    break;
  }

  Prefix.append(": ");
  while (Prefix.size() < 36) {
    Prefix.append(" ");
  }
  OS << Prefix;
}

void CodeCompletionResult::dump() const {
  printPrefix(llvm::errs());
  getCompletionString()->print(llvm::errs());
  llvm::errs() << "\n";
}
