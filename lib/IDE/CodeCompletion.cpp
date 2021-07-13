//===--- CodeCompletion.cpp - Code completion implementation --------------===//
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

#include "swift/IDE/CodeCompletion.h"
#include "CodeCompletionResultBuilder.h"
#include "ExprContextAnalysis.h"
#include "swift/AST/ASTPrinter.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/Comment.h"
#include "swift/AST/ImportCache.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/LazyResolver.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/AST/USRGeneration.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/LLVM.h"
#include "swift/ClangImporter/ClangImporter.h"
#include "swift/ClangImporter/ClangModule.h"
#include "swift/Frontend/FrontendOptions.h"
#include "swift/IDE/CodeCompletionCache.h"
#include "swift/IDE/CodeCompletionResultPrinter.h"
#include "swift/IDE/Utils.h"
#include "swift/Parse/CodeCompletionCallbacks.h"
#include "swift/Sema/IDETypeChecking.h"
#include "swift/Sema/CodeCompletionTypeChecking.h"
#include "swift/Syntax/SyntaxKind.h"
#include "swift/Strings.h"
#include "swift/Subsystems.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Comment.h"
#include "clang/AST/CommentVisitor.h"
#include "clang/AST/Decl.h"
#include "clang/Basic/Module.h"
#include "clang/Index/USRGeneration.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/SaveAndRestore.h"
#include <algorithm>
#include <string>

using namespace swift;
using namespace ide;

using CommandWordsPairs = std::vector<std::pair<StringRef, StringRef>>;
using NotRecommendedReason = CodeCompletionResult::NotRecommendedReason;

enum CodeCompletionCommandKind {
  none,
  keyword,
  recommended,
  recommendedover,
  mutatingvariant,
  nonmutatingvariant,
};

CodeCompletionCommandKind getCommandKind(StringRef Command) {
#define CHECK_CASE(KIND)                                                      \
  if (Command == #KIND)                                                       \
    return CodeCompletionCommandKind::KIND;
  CHECK_CASE(keyword);
  CHECK_CASE(recommended);
  CHECK_CASE(recommendedover);
  CHECK_CASE(mutatingvariant);
  CHECK_CASE(nonmutatingvariant);
#undef CHECK_CASE
  return CodeCompletionCommandKind::none;
}

StringRef getCommandName(CodeCompletionCommandKind Kind) {
#define CHECK_CASE(KIND)                                                    \
  if (CodeCompletionCommandKind::KIND == Kind) {                            \
    static std::string Name(#KIND);                                         \
    return Name;                                                            \
  }
  CHECK_CASE(keyword)
  CHECK_CASE(recommended)
  CHECK_CASE(recommendedover)
  CHECK_CASE(mutatingvariant);
  CHECK_CASE(nonmutatingvariant);
#undef CHECK_CASE
  llvm_unreachable("Cannot handle this Kind.");
}

bool containsInterestedWords(StringRef Content, StringRef Splitter,
                             bool AllowWhitespace) {
  do {
    Content = Content.split(Splitter).second;
    Content = AllowWhitespace ? Content.trim() : Content;
#define CHECK_CASE(KIND)                                                       \
if (Content.startswith(#KIND))                                                 \
return true;
    CHECK_CASE(keyword)
    CHECK_CASE(recommended)
    CHECK_CASE(recommendedover)
    CHECK_CASE(mutatingvariant);
    CHECK_CASE(nonmutatingvariant);
#undef CHECK_CASE
  } while (!Content.empty());
  return false;
}

void splitTextByComma(StringRef Text, std::vector<StringRef>& Subs) {
  do {
    auto Pair = Text.split(',');
    auto Key = Pair.first.trim();
    if (!Key.empty())
      Subs.push_back(Key);
    Text = Pair.second;
  } while (!Text.empty());
}

namespace clang {
namespace comments {
class WordPairsArrangedViewer {
  ArrayRef<std::pair<StringRef, StringRef>> Content;
  std::vector<StringRef> ViewedText;
  std::vector<StringRef> Words;
  StringRef Key;

  bool isKeyViewed(StringRef K) {
    return std::find(ViewedText.begin(), ViewedText.end(), K) != ViewedText.end();
  }

public:
  WordPairsArrangedViewer(ArrayRef<std::pair<StringRef, StringRef>> Content):
    Content(Content) {}

  bool hasNext() {
    Words.clear();
    bool Found = false;
    for (auto P : Content) {
      if (!Found && !isKeyViewed(P.first)) {
        Key = P.first;
        Found = true;
      }
      if (Found && P.first == Key)
        Words.push_back(P.second);
    }
    return Found;
  }

  std::pair<StringRef, ArrayRef<StringRef>> next() {
    bool HasNext = hasNext();
    (void) HasNext;
    assert(HasNext && "Have no more data.");
    ViewedText.push_back(Key);
    return std::make_pair(Key, llvm::makeArrayRef(Words));
  }
};

class ClangCommentExtractor : public ConstCommentVisitor<ClangCommentExtractor> {
  CommandWordsPairs &Words;
  const CommandTraits &Traits;
  std::vector<const Comment *> Parents;

  void visitChildren(const Comment* C) {
    Parents.push_back(C);
    for (auto It = C->child_begin(); It != C->child_end(); ++ It)
      visit(*It);
    Parents.pop_back();
  }

public:
  ClangCommentExtractor(CommandWordsPairs &Words,
                        const CommandTraits &Traits) : Words(Words),
                                                       Traits(Traits) {}
#define CHILD_VISIT(NAME) \
  void visit##NAME(const NAME *C) {\
    visitChildren(C);\
  }
  CHILD_VISIT(FullComment)
  CHILD_VISIT(ParagraphComment)
#undef CHILD_VISIT

  void visitInlineCommandComment(const InlineCommandComment *C) {
    auto Command = C->getCommandName(Traits);
    auto CommandKind = getCommandKind(Command);
    if (CommandKind == CodeCompletionCommandKind::none)
      return;
    auto &Parent = Parents.back();
    for (auto CIT = std::find(Parent->child_begin(), Parent->child_end(), C) + 1;
         CIT != Parent->child_end(); ++CIT) {
      if (auto TC = dyn_cast<TextComment>(*CIT)) {
        auto Text = TC->getText();
        std::vector<StringRef> Subs;
        splitTextByComma(Text, Subs);
        auto Kind = getCommandName(CommandKind);
        for (auto S : Subs)
          Words.push_back(std::make_pair(Kind, S));
      } else
        break;
    }
  }
};

void getClangDocKeyword(ClangImporter &Importer, const Decl *D,
                        CommandWordsPairs &Words) {
  ClangCommentExtractor Extractor(Words, Importer.getClangASTContext().
    getCommentCommandTraits());
  if (auto RC = Importer.getClangASTContext().getRawCommentForAnyRedecl(D)) {
    auto RT = RC->getRawText(Importer.getClangASTContext().getSourceManager());
    if (containsInterestedWords(RT, "@", /*AllowWhitespace*/false)) {
      FullComment* Comment = Importer.getClangASTContext().
        getLocalCommentForDeclUncached(D);
      Extractor.visit(Comment);
    }
  }
}
} // end namespace comments
} // end namespace clang

namespace swift {
namespace markup {
class SwiftDocWordExtractor : public MarkupASTWalker {
  CommandWordsPairs &Pairs;
  CodeCompletionCommandKind Kind;
public:
  SwiftDocWordExtractor(CommandWordsPairs &Pairs) :
    Pairs(Pairs), Kind(CodeCompletionCommandKind::none) {}
  void visitKeywordField(const KeywordField *Field) override {
    Kind = CodeCompletionCommandKind::keyword;
  }
  void visitRecommendedField(const RecommendedField *Field) override {
    Kind = CodeCompletionCommandKind::recommended;
  }
  void visitRecommendedoverField(const RecommendedoverField *Field) override {
    Kind = CodeCompletionCommandKind::recommendedover;
  }
  void visitMutatingvariantField(const MutatingvariantField *Field) override {
    Kind = CodeCompletionCommandKind::mutatingvariant;
  }
  void visitNonmutatingvariantField(const NonmutatingvariantField *Field) override {
    Kind = CodeCompletionCommandKind::nonmutatingvariant;
  }
  void visitText(const Text *Text) override {
    if (Kind == CodeCompletionCommandKind::none)
      return;
    StringRef CommandName = getCommandName(Kind);
    std::vector<StringRef> Subs;
    splitTextByComma(Text->str(), Subs);
    for (auto S : Subs)
      Pairs.push_back(std::make_pair(CommandName, S));
  }
};

void getSwiftDocKeyword(const Decl* D, CommandWordsPairs &Words) {
  auto Interested = false;
  for (auto C : D->getRawComment(/*SerializedOK=*/false).Comments) {
    if (containsInterestedWords(C.RawText, "-", /*AllowWhitespace*/true)) {
      Interested = true;
      break;
    }
  }
  if (!Interested)
    return;
  static swift::markup::MarkupContext MC;
  auto DC = getSingleDocComment(MC, D);
  if (!DC)
    return;
  SwiftDocWordExtractor Extractor(Words);
  for (auto Part : DC->getBodyNodes()) {
    switch (Part->getKind()) {
      case ASTNodeKind::KeywordField:
      case ASTNodeKind::RecommendedField:
      case ASTNodeKind::RecommendedoverField:
      case ASTNodeKind::MutatingvariantField:
      case ASTNodeKind::NonmutatingvariantField:
        Extractor.walk(Part);
        break;
      default:
        break;
    }
  }
}
} // end namespace markup
} // end namespace swift

using DeclFilter = std::function<bool(ValueDecl *, DeclVisibilityKind)>;
static bool DefaultFilter(ValueDecl* VD, DeclVisibilityKind Kind) {
  return true;
}
static bool KeyPathFilter(ValueDecl* decl, DeclVisibilityKind) {
  return isa<TypeDecl>(decl) ||
         (isa<VarDecl>(decl) && decl->getDeclContext()->isTypeContext());
}

static bool SwiftKeyPathFilter(ValueDecl* decl, DeclVisibilityKind) {
  switch(decl->getKind()){
  case DeclKind::Var:
  case DeclKind::Subscript:
    return true;
  default:
    return false;
  }
}

std::string swift::ide::removeCodeCompletionTokens(
    StringRef Input, StringRef TokenName, unsigned *CompletionOffset) {
  assert(TokenName.size() >= 1);

  *CompletionOffset = ~0U;

  std::string CleanFile;
  CleanFile.reserve(Input.size());
  const std::string Token = std::string("#^") + TokenName.str() + "^#";

  for (const char *Ptr = Input.begin(), *End = Input.end();
       Ptr != End; ++Ptr) {
    const char C = *Ptr;
    if (C == '#' && Ptr <= End - Token.size() &&
        StringRef(Ptr, Token.size()) == Token) {
      Ptr += Token.size() - 1;
      *CompletionOffset = CleanFile.size();
      CleanFile += '\0';
      continue;
    }
    if (C == '#' && Ptr <= End - 2 && Ptr[1] == '^') {
      do {
        ++Ptr;
      } while (Ptr < End && *Ptr != '#');
      if (Ptr == End)
        break;
      continue;
    }
    CleanFile += C;
  }
  return CleanFile;
}

llvm::StringRef swift::ide::copyString(llvm::BumpPtrAllocator &Allocator,
                                       llvm::StringRef Str) {
  char *Buffer = Allocator.Allocate<char>(Str.size());
  std::copy(Str.begin(), Str.end(), Buffer);
  return llvm::StringRef(Buffer, Str.size());
}

const char *swift::ide::copyCString(llvm::BumpPtrAllocator &Allocator,
                                    llvm::StringRef Str) {
  char *Buffer = Allocator.Allocate<char>(Str.size() + 1);
  std::copy(Str.begin(), Str.end(), Buffer);
  Buffer[Str.size()] = '\0';
  return Buffer;
}

CodeCompletionString::CodeCompletionString(ArrayRef<Chunk> Chunks) {
  std::uninitialized_copy(Chunks.begin(), Chunks.end(),
                          getTrailingObjects<Chunk>());
  NumChunks = Chunks.size();
}

CodeCompletionString *CodeCompletionString::create(llvm::BumpPtrAllocator &Allocator,
                                                   ArrayRef<Chunk> Chunks) {
  void *CCSMem = Allocator.Allocate(totalSizeToAlloc<Chunk>(Chunks.size()),
                                    alignof(CodeCompletionString));
  return new (CCSMem) CodeCompletionString(Chunks);
}

void CodeCompletionString::print(raw_ostream &OS) const {
  unsigned PrevNestingLevel = 0;
  auto chunks = getChunks();
  for (auto I = chunks.begin(), E = chunks.end(); I != E; ++I) {
    bool AnnotatedTextChunk = false;

    if (I->getNestingLevel() < PrevNestingLevel) {
      OS << "#}";
    }
    switch (I->getKind()) {
    using ChunkKind = Chunk::ChunkKind;
    case ChunkKind::AccessControlKeyword:
    case ChunkKind::DeclAttrKeyword:
    case ChunkKind::DeclAttrParamKeyword:
    case ChunkKind::OverrideKeyword:
    case ChunkKind::EffectsSpecifierKeyword:
    case ChunkKind::DeclIntroducer:
    case ChunkKind::Text:
    case ChunkKind::LeftParen:
    case ChunkKind::RightParen:
    case ChunkKind::LeftBracket:
    case ChunkKind::RightBracket:
    case ChunkKind::LeftAngle:
    case ChunkKind::RightAngle:
    case ChunkKind::Dot:
    case ChunkKind::Ellipsis:
    case ChunkKind::Comma:
    case ChunkKind::ExclamationMark:
    case ChunkKind::QuestionMark:
    case ChunkKind::Ampersand:
    case ChunkKind::Equal:
    case ChunkKind::Whitespace:
    case ChunkKind::Keyword:
    case ChunkKind::Attribute:
    case ChunkKind::BaseName:
    case ChunkKind::TypeIdSystem:
    case ChunkKind::TypeIdUser:
      AnnotatedTextChunk = I->isAnnotation();
      LLVM_FALLTHROUGH;
    case ChunkKind::CallParameterName:
    case ChunkKind::CallParameterInternalName:
    case ChunkKind::CallParameterColon:
    case ChunkKind::DeclAttrParamColon:
    case ChunkKind::CallParameterType:
    case ChunkKind::CallParameterClosureType:
    case ChunkKind::GenericParameterName:
      if (AnnotatedTextChunk)
        OS << "['";
      else if (I->getKind() == ChunkKind::CallParameterInternalName)
        OS << "(";
      else if (I->getKind() == ChunkKind::CallParameterClosureType)
        OS << "##";
      for (char Ch : I->getText()) {
        if (Ch == '\n')
          OS << "\\n";
        else
          OS << Ch;
      }
      if (AnnotatedTextChunk)
        OS << "']";
      else if (I->getKind() == ChunkKind::CallParameterInternalName)
        OS << ")";
      break;
    case ChunkKind::OptionalBegin:
    case ChunkKind::CallParameterBegin:
    case ChunkKind::CallParameterTypeBegin:
    case ChunkKind::GenericParameterBegin:
      OS << "{#";
      break;
    case ChunkKind::DynamicLookupMethodCallTail:
    case ChunkKind::OptionalMethodCallTail:
      OS << I->getText();
      break;
    case ChunkKind::TypeAnnotationBegin: {
      OS << "[#";
      ++I;
      auto level = I->getNestingLevel();
      for (; I != E && !I->endsPreviousNestedGroup(level); ++I)
        if (I->hasText())
          OS << I->getText();
      --I;
      OS << "#]";
      continue;
    }
    case ChunkKind::TypeAnnotation:
      OS << "[#";
      OS << I->getText();
      OS << "#]";
      break;
    case ChunkKind::CallParameterClosureExpr:
      OS << " {" << I->getText() << "|}";
      break;
    case ChunkKind::BraceStmtWithCursor:
      OS << " {|}";
      break;
    }
    PrevNestingLevel = I->getNestingLevel();
  }
  while (PrevNestingLevel > 0) {
    OS << "#}";
    --PrevNestingLevel;
  }
}

void CodeCompletionString::dump() const {
  print(llvm::errs());
}

CodeCompletionDeclKind
CodeCompletionResult::getCodeCompletionDeclKind(const Decl *D) {
  switch (D->getKind()) {
  case DeclKind::Import:
  case DeclKind::Extension:
  case DeclKind::PatternBinding:
  case DeclKind::EnumCase:
  case DeclKind::TopLevelCode:
  case DeclKind::IfConfig:
  case DeclKind::PoundDiagnostic:
  case DeclKind::MissingMember:
  case DeclKind::OpaqueType:
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
    return CodeCompletionDeclKind::Class;
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
  }
  llvm_unreachable("invalid DeclKind");
}

bool CodeCompletionResult::getDeclIsSystem(const Decl *D) {
  return D->getModuleContext()->isSystemModule();
}

void CodeCompletionResult::printPrefix(raw_ostream &OS) const {
  llvm::SmallString<64> Prefix;
  switch (getKind()) {
  case ResultKind::Declaration:
    Prefix.append("Decl");
    switch (getAssociatedDeclKind()) {
    case CodeCompletionDeclKind::Class:
      Prefix.append("[Class]");
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
    }
    break;
  case ResultKind::Keyword:
    Prefix.append("Keyword");
    switch (getKeywordKind()) {
    case CodeCompletionKeywordKind::None:
      break;
#define KEYWORD(X) case CodeCompletionKeywordKind::kw_##X: \
      Prefix.append("[" #X "]"); \
      break;
#define POUND_KEYWORD(X) case CodeCompletionKeywordKind::pound_##X: \
      Prefix.append("[#" #X "]"); \
      break;
#include "swift/Syntax/TokenKinds.def"
    }
    break;
  case ResultKind::Pattern:
    Prefix.append("Pattern");
    break;
  case ResultKind::Literal:
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
  case ResultKind::BuiltinOperator:
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
    if (!ModuleName.empty())
      Prefix.append((Twine("[") + ModuleName + "]").str());
    break;
  }
  if (getFlair().toRaw()) {
    Prefix.append("/Flair[");
    bool isFirstFlair = true;
#define PRINT_FLAIR(KIND, NAME) \
    if (getFlair().contains(CodeCompletionFlairBit::KIND)) { \
      if (isFirstFlair) { isFirstFlair = false; } \
      else { Prefix.append(","); } \
      Prefix.append(NAME); \
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
  if (NotRecommended)
    Prefix.append("/NotRecommended");
  if (IsSystem)
    Prefix.append("/IsSystem");
  if (NumBytesToErase != 0) {
    Prefix.append("/Erase[");
    Prefix.append(Twine(NumBytesToErase).str());
    Prefix.append("]");
  }
  switch (getExpectedTypeRelation()) {
    case ExpectedTypeRelation::Invalid:
      Prefix.append("/TypeRelation[Invalid]");
      break;
    case ExpectedTypeRelation::Identical:
      Prefix.append("/TypeRelation[Identical]");
      break;
    case ExpectedTypeRelation::Convertible:
      Prefix.append("/TypeRelation[Convertible]");
      break;
    case ExpectedTypeRelation::NotApplicable:
    case ExpectedTypeRelation::Unknown:
    case ExpectedTypeRelation::Unrelated:
      break;
  }

  for (clang::comments::WordPairsArrangedViewer Viewer(DocWords);
       Viewer.hasNext();) {
    auto Pair = Viewer.next();
    Prefix.append("/");
    Prefix.append(Pair.first);
    Prefix.append("[");
    StringRef Sep = ", ";
    for (auto KW : Pair.second) {
      Prefix.append(KW);
      Prefix.append(Sep);
    }
    for (unsigned I = 0, N = Sep.size(); I < N; ++I)
      Prefix.pop_back();
    Prefix.append("]");
  }

  Prefix.append(": ");
  while (Prefix.size() < 36) {
    Prefix.append(" ");
  }
  OS << Prefix;
}

void CodeCompletionResult::dump() const {
  printPrefix(llvm::errs());
  CompletionString->print(llvm::errs());
  llvm::errs() << "\n";
}

CodeCompletionResult *
CodeCompletionResult::withFlair(CodeCompletionFlair newFlair,
                                CodeCompletionResultSink &Sink) {
  if (Kind == ResultKind::Declaration) {
    return new (*Sink.Allocator) CodeCompletionResult(
        getSemanticContext(), newFlair, getNumBytesToErase(),
        getCompletionString(), getAssociatedDeclKind(), isSystem(),
        getModuleName(), getNotRecommendedReason(), getBriefDocComment(),
        getAssociatedUSRs(), getDeclKeywords(), getExpectedTypeRelation(),
        isOperator() ? getOperatorKind() : CodeCompletionOperatorKind::None);
  } else {
    return new (*Sink.Allocator) CodeCompletionResult(
        getKind(), getSemanticContext(), newFlair, getNumBytesToErase(),
        getCompletionString(), getExpectedTypeRelation(),
        isOperator() ? getOperatorKind() : CodeCompletionOperatorKind::None);
  }
}

void CodeCompletionResultBuilder::withNestedGroup(
    CodeCompletionString::Chunk::ChunkKind Kind,
    llvm::function_ref<void()> body) {
  ++CurrentNestingLevel;
  addSimpleChunk(Kind);
  body();
  --CurrentNestingLevel;
}

void CodeCompletionResultBuilder::addChunkWithText(
    CodeCompletionString::Chunk::ChunkKind Kind, StringRef Text) {
  addChunkWithTextNoCopy(Kind, copyString(*Sink.Allocator, Text));
}

void CodeCompletionResultBuilder::setAssociatedDecl(const Decl *D) {
  assert(Kind == CodeCompletionResult::ResultKind::Declaration);
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
    setNotRecommended(NotRecommendedReason::Deprecated);
}

namespace {
class AnnotatedTypePrinter : public ASTPrinter {
  using ChunkKind = CodeCompletionString::Chunk::ChunkKind;
  CodeCompletionResultBuilder &Builder;
  SmallString<16> Buffer;
  ChunkKind CurrChunkKind = ChunkKind::Text;
  ChunkKind NextChunkKind = ChunkKind::Text;

  Optional<ChunkKind> getChunkKindForPrintNameContext(PrintNameContext context) {
    switch (context) {
    case PrintNameContext::Keyword:
      return ChunkKind::Keyword;
    case PrintNameContext::Attribute:
      return ChunkKind::Attribute;
    default:
      return None;
    }
  }

  void flush() {
    if (Buffer.empty())
      return;
    Builder.addChunkWithText(CurrChunkKind, Buffer);
    Buffer.clear();
  }

public:
  AnnotatedTypePrinter(CodeCompletionResultBuilder &Builder) : Builder(Builder) {}

  ~AnnotatedTypePrinter() {
    // Flush the remainings.
    flush();
  }

  void printText(StringRef Text) override {
    if (CurrChunkKind != NextChunkKind) {
      // If the next desired kind is different from the current buffer, flush
      // the current buffer.
      flush();
      CurrChunkKind = NextChunkKind;
    }
    Buffer.append(Text);
  }

  void printTypeRef(
      Type T, const TypeDecl *TD, Identifier Name,
      PrintNameContext NameContext = PrintNameContext::Normal) override {

    NextChunkKind = TD->getModuleContext()->isSystemModule()
      ? ChunkKind::TypeIdSystem
      : ChunkKind::TypeIdUser;

    ASTPrinter::printTypeRef(T, TD, Name, NameContext);
    NextChunkKind = ChunkKind::Text;
  }

  void printNamePre(PrintNameContext context) override {
    if (auto Kind = getChunkKindForPrintNameContext(context))
      NextChunkKind = *Kind;
  }

  void printNamePost(PrintNameContext context) override {
    if (auto Kind = getChunkKindForPrintNameContext(context))
      NextChunkKind = ChunkKind::Text;
  }
};
} // namespcae

void CodeCompletionResultBuilder::addCallParameter(Identifier Name,
                                                   Identifier LocalName,
                                                   Type Ty,
                                                   Type ContextTy,
                                                   bool IsVarArg,
                                                   bool IsInOut,
                                                   bool IsIUO,
                                                   bool isAutoClosure,
                                                   bool useUnderscoreLabel,
                                                   bool isLabeledTrailingClosure) {
  ++CurrentNestingLevel;
  using ChunkKind = CodeCompletionString::Chunk::ChunkKind;

  addSimpleChunk(ChunkKind::CallParameterBegin);

  if (shouldAnnotateResults()) {
    if (!Name.empty() || !LocalName.empty()) {
      llvm::SmallString<16> EscapedKeyword;

      if (!Name.empty()) {
        addChunkWithText(
            CodeCompletionString::Chunk::ChunkKind::CallParameterName,
            escapeKeyword(Name.str(), false, EscapedKeyword));

        if (!LocalName.empty() && Name != LocalName) {
          addChunkWithTextNoCopy(ChunkKind::Text, " ");
          getLastChunk().setIsAnnotation();
          addChunkWithText(ChunkKind::CallParameterInternalName,
              escapeKeyword(LocalName.str(), false, EscapedKeyword));
          getLastChunk().setIsAnnotation();
        }
      } else {
        assert(!LocalName.empty());
        addChunkWithTextNoCopy(ChunkKind::CallParameterName, "_");
        getLastChunk().setIsAnnotation();
        addChunkWithTextNoCopy(ChunkKind::Text, " ");
        getLastChunk().setIsAnnotation();
        addChunkWithText(ChunkKind::CallParameterInternalName,
            escapeKeyword(LocalName.str(), false, EscapedKeyword));
      }
      addChunkWithTextNoCopy(ChunkKind::CallParameterColon, ": ");
    }
  } else {
    if (!Name.empty()) {
      llvm::SmallString<16> EscapedKeyword;
      addChunkWithText(
          CodeCompletionString::Chunk::ChunkKind::CallParameterName,
          escapeKeyword(Name.str(), false, EscapedKeyword));
      addChunkWithTextNoCopy(
          CodeCompletionString::Chunk::ChunkKind::CallParameterColon, ": ");
    } else if (useUnderscoreLabel) {
      addChunkWithTextNoCopy(
          CodeCompletionString::Chunk::ChunkKind::CallParameterName, "_");
      addChunkWithTextNoCopy(
          CodeCompletionString::Chunk::ChunkKind::CallParameterColon, ": ");
    } else if (!LocalName.empty()) {
      // Use local (non-API) parameter name if we have nothing else.
      llvm::SmallString<16> EscapedKeyword;
      addChunkWithText(
          CodeCompletionString::Chunk::ChunkKind::CallParameterInternalName,
            escapeKeyword(LocalName.str(), false, EscapedKeyword));
      addChunkWithTextNoCopy(
          CodeCompletionString::Chunk::ChunkKind::CallParameterColon, ": ");
    }
  }

  // 'inout' arguments are printed specially.
  if (IsInOut) {
    addChunkWithTextNoCopy(
        CodeCompletionString::Chunk::ChunkKind::Ampersand, "&");
    Ty = Ty->getInOutObjectType();
  }

  // If the parameter is of the type @autoclosure ()->output, then the
  // code completion should show the parameter of the output type
  // instead of the function type ()->output.
  if (isAutoClosure) {
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
    withNestedGroup(ChunkKind::CallParameterTypeBegin, [&]() {
      AnnotatedTypePrinter printer(*this);
      Ty->print(printer, PO);
    });
  } else {
    std::string TypeName = Ty->getString(PO);
    addChunkWithText(ChunkKind::CallParameterType, TypeName);
  }

  // Look through optional types and type aliases to find out if we have
  // function type.
  Ty = Ty->lookThroughAllOptionalTypes();
  if (auto AFT = Ty->getAs<AnyFunctionType>()) {
    // If this is a closure type, add ChunkKind::CallParameterClosureType or
    // ChunkKind::CallParameterClosureExpr for labeled trailing closures.
    PrintOptions PO;
    PO.PrintFunctionRepresentationAttrs =
      PrintOptions::FunctionRepresentationMode::None;
    PO.SkipAttributes = true;
    PO.OpaqueReturnTypePrinting =
        PrintOptions::OpaqueReturnTypePrintingMode::WithoutOpaqueKeyword;
    PO.AlwaysTryPrintParameterLabels = true;
    if (ContextTy)
      PO.setBaseType(ContextTy);

    if (isLabeledTrailingClosure) {
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
         CodeCompletionString::Chunk::ChunkKind::CallParameterClosureExpr,
         OS.str());
    } else {
      // Add the closure type.
      addChunkWithText(
          CodeCompletionString::Chunk::ChunkKind::CallParameterClosureType,
          AFT->getString(PO));
    }
  }

  if (IsVarArg)
    addEllipsis();
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
                      AnnotatedTypePrinter printer(*this);
                      T->print(printer, PO);
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

StringRef CodeCompletionContext::copyString(StringRef Str) {
  return ::copyString(*CurrentResults.Allocator, Str);
}

bool shouldCopyAssociatedUSRForDecl(const ValueDecl *VD) {
  // Avoid trying to generate a USR for some declaration types.
  if (isa<AbstractTypeParamDecl>(VD) && !isa<AssociatedTypeDecl>(VD))
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

ArrayRef<StringRef> copyAssociatedUSRs(llvm::BumpPtrAllocator &Allocator,
                                       const Decl *D) {
  llvm::SmallVector<StringRef, 4> USRs;
  walkValueDeclAndOverriddenDecls(D, [&](llvm::PointerUnion<const ValueDecl*,
                                                  const clang::NamedDecl*> OD) {
    llvm::SmallString<128> SS;
    bool Ignored = true;
    if (auto *OVD = OD.dyn_cast<const ValueDecl*>()) {
      if (shouldCopyAssociatedUSRForDecl(OVD)) {
        llvm::raw_svector_ostream OS(SS);
        Ignored = printValueDeclUSR(OVD, OS);
      }
    } else if (auto *OND = OD.dyn_cast<const clang::NamedDecl*>()) {
      Ignored = clang::index::generateUSRForDecl(OND, SS);
    }

    if (!Ignored)
      USRs.push_back(copyString(Allocator, SS));
  });

  if (!USRs.empty())
    return copyArray(Allocator, ArrayRef<StringRef>(USRs));

  return ArrayRef<StringRef>();
}

static CodeCompletionResult::ExpectedTypeRelation
calculateTypeRelation(Type Ty, Type ExpectedTy, const DeclContext *DC) {
  if (Ty.isNull() || ExpectedTy.isNull() ||
      Ty->is<ErrorType>() ||
      ExpectedTy->is<ErrorType>())
    return CodeCompletionResult::ExpectedTypeRelation::Unrelated;

  // Equality/Conversion of GenericTypeParameterType won't account for
  // requirements – ignore them
  if (!Ty->hasTypeParameter() && !ExpectedTy->hasTypeParameter()) {
    if (Ty->isEqual(ExpectedTy))
      return CodeCompletionResult::ExpectedTypeRelation::Identical;
    bool isAny = false;
    isAny |= ExpectedTy->isAny();
    isAny |= ExpectedTy->is<ArchetypeType>() &&
             !ExpectedTy->castTo<ArchetypeType>()->hasRequirements();

    if (!isAny && isConvertibleTo(Ty, ExpectedTy, /*openArchetypes=*/true,
                                  *const_cast<DeclContext *>(DC)))
      return CodeCompletionResult::ExpectedTypeRelation::Convertible;
  }
  if (auto FT = Ty->getAs<AnyFunctionType>()) {
    if (FT->getResult()->isVoid())
      return CodeCompletionResult::ExpectedTypeRelation::Invalid;
  }
  return CodeCompletionResult::ExpectedTypeRelation::Unrelated;
}

static CodeCompletionResult::ExpectedTypeRelation
calculateMaxTypeRelation(Type Ty, const ExpectedTypeContext &typeContext,
                         const DeclContext *DC) {
  if (typeContext.empty())
    return CodeCompletionResult::ExpectedTypeRelation::Unknown;

  if (auto funcTy = Ty->getAs<AnyFunctionType>())
    Ty = funcTy->removeArgumentLabels(1);

  auto Result = CodeCompletionResult::ExpectedTypeRelation::Unrelated;
  for (auto expectedTy : typeContext.possibleTypes) {
    // Do not use Void type context for a single-expression body, since the
    // implicit return does not constrain the expression.
    //
    //     { ... -> ()  in x } // x can be anything
    //
    // This behaves differently from explicit return, and from non-Void:
    //
    //     { ... -> Int in x }        // x must be Int
    //     { ... -> ()  in return x } // x must be Void
    if (typeContext.isImplicitSingleExpressionReturn && expectedTy->isVoid())
      continue;

    Result = std::max(Result, calculateTypeRelation(Ty, expectedTy, DC));

    // Map invalid -> unrelated when in a single-expression body, since the
    // input may be incomplete.
    if (typeContext.isImplicitSingleExpressionReturn &&
        Result == CodeCompletionResult::ExpectedTypeRelation::Invalid)
      Result = CodeCompletionResult::ExpectedTypeRelation::Unrelated;
  }
  return Result;
}

CodeCompletionOperatorKind
CodeCompletionResult::getCodeCompletionOperatorKind(StringRef name) {
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

static StringRef getOperatorName(CodeCompletionString *str) {
  return str->getFirstTextChunk(/*includeLeadingPunctuation=*/true);
}

CodeCompletionOperatorKind
CodeCompletionResult::getCodeCompletionOperatorKind(CodeCompletionString *str) {
  return getCodeCompletionOperatorKind(getOperatorName(str));
}

CodeCompletionResult *CodeCompletionResultBuilder::takeResult() {
  auto *CCS = CodeCompletionString::create(*Sink.Allocator, Chunks);

  switch (Kind) {
  case CodeCompletionResult::ResultKind::Declaration: {
    StringRef BriefComment;
    auto MaybeClangNode = AssociatedDecl->getClangNode();
    if (MaybeClangNode) {
      if (auto *D = MaybeClangNode.getAsDecl()) {
        const auto &ClangContext = D->getASTContext();
        if (const clang::RawComment *RC =
                ClangContext.getRawCommentForAnyRedecl(D))
          BriefComment = RC->getBriefText(ClangContext);
      }
    } else {
      BriefComment = AssociatedDecl->getBriefComment();
    }

    StringRef ModuleName;
    if (CurrentModule) {
      if (Sink.LastModule.first == CurrentModule.getOpaqueValue()) {
        ModuleName = Sink.LastModule.second;
      } else {
        if (auto *C = CurrentModule.dyn_cast<const clang::Module *>()) {
          ModuleName = copyString(*Sink.Allocator, C->getFullModuleName());
        } else {
          ModuleName = copyString(
              *Sink.Allocator,
              CurrentModule.get<const swift::ModuleDecl *>()->getName().str());
        }
        Sink.LastModule.first = CurrentModule.getOpaqueValue();
        Sink.LastModule.second = ModuleName;
      }
    }

    return new (*Sink.Allocator) CodeCompletionResult(
        SemanticContext, Flair, NumBytesToErase, CCS, AssociatedDecl,
        ModuleName, NotRecReason, copyString(*Sink.Allocator, BriefComment),
        copyAssociatedUSRs(*Sink.Allocator, AssociatedDecl),
        copyArray(*Sink.Allocator, CommentWords), ExpectedTypeRelation);
  }

  case CodeCompletionResult::ResultKind::Keyword:
    return new (*Sink.Allocator)
        CodeCompletionResult(
          KeywordKind, SemanticContext, Flair, NumBytesToErase,
          CCS, ExpectedTypeRelation,
          copyString(*Sink.Allocator, BriefDocComment));

  case CodeCompletionResult::ResultKind::BuiltinOperator:
  case CodeCompletionResult::ResultKind::Pattern:
    return new (*Sink.Allocator) CodeCompletionResult(
        Kind, SemanticContext, Flair, NumBytesToErase, CCS,
        ExpectedTypeRelation, CodeCompletionOperatorKind::None,
        copyString(*Sink.Allocator, BriefDocComment));

  case CodeCompletionResult::ResultKind::Literal:
    assert(LiteralKind.hasValue());
    return new (*Sink.Allocator)
        CodeCompletionResult(*LiteralKind, SemanticContext, Flair,
                             NumBytesToErase, CCS, ExpectedTypeRelation);
  }

  llvm_unreachable("Unhandled CodeCompletionResult in switch.");
}

void CodeCompletionResultBuilder::finishResult() {
  if (!Cancelled)
    Sink.Results.push_back(takeResult());
}


MutableArrayRef<CodeCompletionResult *> CodeCompletionContext::takeResults() {
  // Copy pointers to the results.
  const size_t Count = CurrentResults.Results.size();
  CodeCompletionResult **Results =
      CurrentResults.Allocator->Allocate<CodeCompletionResult *>(Count);
  std::copy(CurrentResults.Results.begin(), CurrentResults.Results.end(),
            Results);
  CurrentResults.Results.clear();
  return MutableArrayRef<CodeCompletionResult *>(Results, Count);
}

Optional<unsigned> CodeCompletionString::getFirstTextChunkIndex(
    bool includeLeadingPunctuation) const {
  for (auto i : indices(getChunks())) {
    auto &C = getChunks()[i];
    switch (C.getKind()) {
    using ChunkKind = Chunk::ChunkKind;
    case ChunkKind::Text:
    case ChunkKind::CallParameterName:
    case ChunkKind::CallParameterInternalName:
    case ChunkKind::GenericParameterName:
    case ChunkKind::LeftParen:
    case ChunkKind::LeftBracket:
    case ChunkKind::Equal:
    case ChunkKind::DeclAttrParamKeyword:
    case ChunkKind::DeclAttrKeyword:
    case ChunkKind::Keyword:
    case ChunkKind::Attribute:
    case ChunkKind::BaseName:
    case ChunkKind::TypeIdSystem:
    case ChunkKind::TypeIdUser:
    case ChunkKind::CallParameterBegin:
      return i;
    case ChunkKind::Dot:
    case ChunkKind::ExclamationMark:
    case ChunkKind::QuestionMark:
      if (includeLeadingPunctuation)
        return i;
      continue;
    case ChunkKind::RightParen:
    case ChunkKind::RightBracket:
    case ChunkKind::LeftAngle:
    case ChunkKind::RightAngle:
    case ChunkKind::Ellipsis:
    case ChunkKind::Comma:
    case ChunkKind::Ampersand:
    case ChunkKind::Whitespace:
    case ChunkKind::AccessControlKeyword:
    case ChunkKind::OverrideKeyword:
    case ChunkKind::EffectsSpecifierKeyword:
    case ChunkKind::DeclIntroducer:
    case ChunkKind::CallParameterColon:
    case ChunkKind::CallParameterTypeBegin:
    case ChunkKind::DeclAttrParamColon:
    case ChunkKind::CallParameterType:
    case ChunkKind::CallParameterClosureType:
    case ChunkKind::CallParameterClosureExpr:
    case ChunkKind::OptionalBegin:
    case ChunkKind::GenericParameterBegin:
    case ChunkKind::DynamicLookupMethodCallTail:
    case ChunkKind::OptionalMethodCallTail:
    case ChunkKind::TypeAnnotation:
    case ChunkKind::TypeAnnotationBegin:
      continue;

    case ChunkKind::BraceStmtWithCursor:
      llvm_unreachable("should have already extracted the text");
    }
  }
  return None;
}

StringRef
CodeCompletionString::getFirstTextChunk(bool includeLeadingPunctuation) const {
  Optional<unsigned> Idx = getFirstTextChunkIndex(includeLeadingPunctuation);
  if (Idx.hasValue())
    return getChunks()[*Idx].getText();
  return StringRef();
}

void CodeCompletionString::getName(raw_ostream &OS) const {
  auto FirstTextChunk = getFirstTextChunkIndex();
  int TextSize = 0;
  if (FirstTextChunk.hasValue()) {
    auto chunks = getChunks().slice(*FirstTextChunk);

    for (auto i = chunks.begin(), e = chunks.end(); i != e; ++i) {
      using ChunkKind = Chunk::ChunkKind;

      bool shouldPrint = !i->isAnnotation();
      switch (i->getKind()) {
      case ChunkKind::TypeAnnotation:
      case ChunkKind::CallParameterClosureType:
      case ChunkKind::CallParameterClosureExpr:
      case ChunkKind::DeclAttrParamColon:
      case ChunkKind::OptionalMethodCallTail:
        continue;
      case ChunkKind::TypeAnnotationBegin: {
        auto level = i->getNestingLevel();
        do { ++i; } while (i != e && !i->endsPreviousNestedGroup(level));
        --i;
        continue;
      }
      case ChunkKind::EffectsSpecifierKeyword:
        shouldPrint = true; // Even when they're annotations.
        break;
      default:
        break;
      }

      if (i->hasText() && shouldPrint) {
        TextSize += i->getText().size();
        OS << i->getText();
      }
    }
  }
}

void CodeCompletionContext::sortCompletionResults(
    MutableArrayRef<CodeCompletionResult *> Results) {
  struct ResultAndName {
    CodeCompletionResult *result;
    std::string name;
  };

  // Caching the name of each field is important to avoid unnecessary calls to
  // CodeCompletionString::getName().
  std::vector<ResultAndName> nameCache(Results.size());
  for (unsigned i = 0, n = Results.size(); i < n; ++i) {
    auto *result = Results[i];
    nameCache[i].result = result;
    llvm::raw_string_ostream OS(nameCache[i].name);
    result->getCompletionString()->getName(OS);
    OS.flush();
  }

  // Sort nameCache, and then transform Results to return the pointers in order.
  std::sort(nameCache.begin(), nameCache.end(),
            [](const ResultAndName &LHS, const ResultAndName &RHS) {
    int Result = StringRef(LHS.name).compare_lower(RHS.name);
    // If the case insensitive comparison is equal, then secondary sort order
    // should be case sensitive.
    if (Result == 0)
      Result = LHS.name.compare(RHS.name);
    return Result < 0;
  });

  llvm::transform(nameCache, Results.begin(),
                  [](const ResultAndName &entry) { return entry.result; });
}

namespace {

class CodeCompletionCallbacksImpl : public CodeCompletionCallbacks {
  CodeCompletionContext &CompletionContext;
  CodeCompletionConsumer &Consumer;
  CodeCompletionExpr *CodeCompleteTokenExpr = nullptr;
  CompletionKind Kind = CompletionKind::None;
  Expr *ParsedExpr = nullptr;
  SourceLoc DotLoc;
  TypeLoc ParsedTypeLoc;
  DeclContext *CurDeclContext = nullptr;
  DeclAttrKind AttrKind;

  /// In situations when \c SyntaxKind hints or determines
  /// completions, i.e. a precedence group attribute, this
  /// can be set and used to control the code completion scenario.
  SyntaxKind SyntxKind;

  int AttrParamIndex;
  bool IsInSil = false;
  bool HasSpace = false;
  bool ShouldCompleteCallPatternAfterParen = true;
  bool PreferFunctionReferencesToCalls = false;
  bool AttTargetIsIndependent = false;
  bool IsAtStartOfLine = false;
  Optional<DeclKind> AttTargetDK;
  Optional<StmtKind> ParentStmtKind;

  SmallVector<StringRef, 3> ParsedKeywords;
  SourceLoc introducerLoc;

  std::vector<std::pair<std::string, bool>> SubModuleNameVisibilityPairs;

  Optional<std::pair<Type, ConcreteDeclRef>> typeCheckParsedExpr() {
    assert(ParsedExpr && "should have an expression");

    // Figure out the kind of type-check we'll be performing.
    auto CheckKind = CompletionTypeCheckKind::Normal;
    if (Kind == CompletionKind::KeyPathExprObjC)
      CheckKind = CompletionTypeCheckKind::KeyPath;

    // If we've already successfully type-checked the expression for some
    // reason, just return the type.
    // FIXME: if it's ErrorType but we've already typechecked we shouldn't
    // typecheck again. rdar://21466394
    if (CheckKind == CompletionTypeCheckKind::Normal &&
        ParsedExpr->getType() && !ParsedExpr->getType()->is<ErrorType>()) {
      return getReferencedDecl(ParsedExpr);
    }

    ConcreteDeclRef ReferencedDecl = nullptr;
    Expr *ModifiedExpr = ParsedExpr;
    if (auto T = getTypeOfCompletionContextExpr(P.Context, CurDeclContext,
                                                CheckKind, ModifiedExpr,
                                                ReferencedDecl)) {
      // FIXME: even though we don't apply the solution, the type checker may
      // modify the original expression. We should understand what effect that
      // may have on code completion.
      ParsedExpr = ModifiedExpr;

      return std::make_pair(*T, ReferencedDecl);
    }
    return None;
  }

  /// \returns true on success, false on failure.
  bool typecheckParsedType() {
    assert(ParsedTypeLoc.getTypeRepr() && "should have a TypeRepr");
    if (ParsedTypeLoc.wasValidated() && !ParsedTypeLoc.isError()) {
      return true;
    }

    const auto ty = swift::performTypeResolution(
        ParsedTypeLoc.getTypeRepr(), P.Context,
        /*isSILMode=*/false,
        /*isSILType=*/false,
        CurDeclContext->getGenericEnvironmentOfContext(),
        /*GenericParams=*/nullptr,
        CurDeclContext,
        /*ProduceDiagnostics=*/false);
    ParsedTypeLoc.setType(ty);
    if (!ParsedTypeLoc.isError()) {
      return true;
    }

    // It doesn't type check as a type, so see if it's a qualifying module name.
    if (auto *ITR = dyn_cast<IdentTypeRepr>(ParsedTypeLoc.getTypeRepr())) {
      const auto &componentRange = ITR->getComponentRange();
      // If it has more than one component, it can't be a module name.
      if (std::distance(componentRange.begin(), componentRange.end()) != 1)
        return false;

      const auto &component = componentRange.front();
      ImportPath::Module::Builder builder(
          component->getNameRef().getBaseIdentifier(),
          component->getLoc());

      if (auto Module = Context.getLoadedModule(builder.get()))
        ParsedTypeLoc.setType(ModuleType::get(Module));
      return true;
    }
    return false;
  }

public:
  CodeCompletionCallbacksImpl(Parser &P,
                              CodeCompletionContext &CompletionContext,
                              CodeCompletionConsumer &Consumer)
      : CodeCompletionCallbacks(P), CompletionContext(CompletionContext),
        Consumer(Consumer) {
  }

  void setAttrTargetDeclKind(Optional<DeclKind> DK) override {
    if (DK == DeclKind::PatternBinding)
      DK = DeclKind::Var;
    else if (DK == DeclKind::Param)
      // For params, consider the attribute is always for the decl.
      AttTargetIsIndependent = false;

    if (!AttTargetIsIndependent)
      AttTargetDK = DK;
  }

  void completeDotExpr(CodeCompletionExpr *E, SourceLoc DotLoc) override;
  void completeStmtOrExpr(CodeCompletionExpr *E) override;
  void completePostfixExprBeginning(CodeCompletionExpr *E) override;
  void completeForEachSequenceBeginning(CodeCompletionExpr *E) override;
  void completePostfixExpr(Expr *E, bool hasSpace) override;
  void completePostfixExprParen(Expr *E, Expr *CodeCompletionE) override;
  void completeExprKeyPath(KeyPathExpr *KPE, SourceLoc DotLoc) override;

  void completeTypeDeclResultBeginning() override;
  void completeTypeSimpleBeginning() override;
  void completeTypeIdentifierWithDot(IdentTypeRepr *ITR) override;
  void completeTypeIdentifierWithoutDot(IdentTypeRepr *ITR) override;

  void completeCaseStmtKeyword() override;
  void completeCaseStmtBeginning(CodeCompletionExpr *E) override;
  void completeDeclAttrBeginning(bool Sil, bool isIndependent) override;
  void completeDeclAttrParam(DeclAttrKind DK, int Index) override;
  void completeEffectsSpecifier(bool hasAsync, bool hasThrows) override;
  void completeInPrecedenceGroup(SyntaxKind SK) override;
  void completeNominalMemberBeginning(
      SmallVectorImpl<StringRef> &Keywords, SourceLoc introducerLoc) override;
  void completeAccessorBeginning(CodeCompletionExpr *E) override;

  void completePoundAvailablePlatform() override;
  void completeImportDecl(ImportPath::Builder &Path) override;
  void completeUnresolvedMember(CodeCompletionExpr *E,
                                SourceLoc DotLoc) override;
  void completeCallArg(CodeCompletionExpr *E, bool isFirst) override;
  void completeLabeledTrailingClosure(CodeCompletionExpr *E,
                                      bool isAtStartOfLine) override;

  bool canPerformCompleteLabeledTrailingClosure() const override {
    return true;
  }

  void completeReturnStmt(CodeCompletionExpr *E) override;
  void completeYieldStmt(CodeCompletionExpr *E,
                         Optional<unsigned> yieldIndex) override;
  void completeAfterPoundExpr(CodeCompletionExpr *E,
                              Optional<StmtKind> ParentKind) override;
  void completeAfterPoundDirective() override;
  void completePlatformCondition() override;
  void completeGenericRequirement() override;
  void completeAfterIfStmt(bool hasElse) override;
  void completeStmtLabel(StmtKind ParentKind) override;
  void completeForEachPatternBeginning(bool hasTry, bool hasAwait) override;
  void completeTypeAttrBeginning() override;

  void doneParsing() override;

private:
  void addKeywords(CodeCompletionResultSink &Sink, bool MaybeFuncBody);
  bool trySolverCompletion(bool MaybeFuncBody);
};
} // end anonymous namespace

namespace {
static bool isTopLevelSubcontext(const DeclContext *DC) {
  for (; DC && DC->isLocalContext(); DC = DC->getParent()) {
    switch (DC->getContextKind()) {
    case DeclContextKind::TopLevelCodeDecl:
      return true;
    case DeclContextKind::AbstractFunctionDecl:
    case DeclContextKind::SubscriptDecl:
    case DeclContextKind::EnumElementDecl:
      return false;
    default:
      continue;
    }
  }
  return false;
}

static KnownProtocolKind
protocolForLiteralKind(CodeCompletionLiteralKind kind) {
  switch (kind) {
  case CodeCompletionLiteralKind::ArrayLiteral:
    return KnownProtocolKind::ExpressibleByArrayLiteral;
  case CodeCompletionLiteralKind::BooleanLiteral:
    return KnownProtocolKind::ExpressibleByBooleanLiteral;
  case CodeCompletionLiteralKind::ColorLiteral:
    return KnownProtocolKind::ExpressibleByColorLiteral;
  case CodeCompletionLiteralKind::ImageLiteral:
    return KnownProtocolKind::ExpressibleByImageLiteral;
  case CodeCompletionLiteralKind::DictionaryLiteral:
    return KnownProtocolKind::ExpressibleByDictionaryLiteral;
  case CodeCompletionLiteralKind::IntegerLiteral:
    return KnownProtocolKind::ExpressibleByIntegerLiteral;
  case CodeCompletionLiteralKind::NilLiteral:
    return KnownProtocolKind::ExpressibleByNilLiteral;
  case CodeCompletionLiteralKind::StringLiteral:
    return KnownProtocolKind::ExpressibleByUnicodeScalarLiteral;
  case CodeCompletionLiteralKind::Tuple:
    llvm_unreachable("no such protocol kind");
  }

  llvm_unreachable("Unhandled CodeCompletionLiteralKind in switch.");
}

static Type
defaultTypeLiteralKind(CodeCompletionLiteralKind kind, ASTContext &Ctx) {
  switch (kind) {
  case CodeCompletionLiteralKind::BooleanLiteral:
    return Ctx.getBoolType();
  case CodeCompletionLiteralKind::IntegerLiteral:
    return Ctx.getIntType();
  case CodeCompletionLiteralKind::StringLiteral:
    return Ctx.getStringType();
  case CodeCompletionLiteralKind::ArrayLiteral:
    return Ctx.getArrayDecl()->getDeclaredType();
  case CodeCompletionLiteralKind::DictionaryLiteral:
    return Ctx.getDictionaryDecl()->getDeclaredType();
  case CodeCompletionLiteralKind::NilLiteral:
  case CodeCompletionLiteralKind::ColorLiteral:
  case CodeCompletionLiteralKind::ImageLiteral:
  case CodeCompletionLiteralKind::Tuple:
    return Type();
  }

  llvm_unreachable("Unhandled CodeCompletionLiteralKind in switch.");
}

/// Whether funcType has a single argument (not including defaulted arguments)
/// that is of type () -> ().
static bool hasTrivialTrailingClosure(const FuncDecl *FD,
                                      AnyFunctionType *funcType) {
  ParameterListInfo paramInfo(funcType->getParams(), FD,
                              /*skipCurriedSelf*/ FD->hasCurriedSelf());

  if (paramInfo.size() - paramInfo.numNonDefaultedParameters() == 1) {
    auto param = funcType->getParams().back();
    if (!param.isAutoClosure()) {
      if (auto Fn = param.getOldType()->getAs<AnyFunctionType>()) {
        return Fn->getParams().empty() && Fn->getResult()->isVoid();
      }
    }
  }

  return false;
}

/// Returns \c true if \p DC can handles async call.
static bool canDeclContextHandleAsync(const DeclContext *DC) {
  if (auto *func = dyn_cast<AbstractFunctionDecl>(DC))
    return func->isAsyncContext();

  if (auto *closure = dyn_cast<ClosureExpr>(DC)) {
    // See if the closure has 'async' function type.
    if (auto closureType = closure->getType())
      if (auto fnType = closureType->getAs<AnyFunctionType>())
        if (fnType->isAsync())
          return true;

    // If the closure doesn't contain any async call in the body, closure itself
    // doesn't have 'async' type even if 'async' closure is expected.
    //   func foo(fn: () async -> Void)
    //   foo { <HERE> }
    // In this case, the closure is wrapped with a 'FunctionConversionExpr'
    // which has 'async' function type.
    struct AsyncClosureChecker: public ASTWalker {
      const ClosureExpr *Target;
      bool Result = false;

      AsyncClosureChecker(const ClosureExpr *Target) : Target(Target) {}

      std::pair<bool, Expr *> walkToExprPre(Expr *E) override {
        if (E == Target)
          return {false, E};

        if (auto conversionExpr = dyn_cast<FunctionConversionExpr>(E)) {
          if (conversionExpr->getSubExpr() == Target) {
            if (conversionExpr->getType()->is<AnyFunctionType>() &&
                conversionExpr->getType()->castTo<AnyFunctionType>()->isAsync())
              Result = true;
            return {false, E};
          }
        }
        return {true, E};
      }
    } checker(closure);
    closure->getParent()->walkContext(checker);
    return checker.Result;
  }

  return false;
}

/// Returns \c true only if the completion is happening for top-level
/// declrarations. i.e.:
///
///     if condition {
///       #false#
///     }
///     expr.#false#
///
///     #true#
///
///     struct S {
///       #false#
///       func foo() {
///         #false#
///       }
///     }
static bool isCodeCompletionAtTopLevel(const DeclContext *DC) {
  if (DC->isModuleScopeContext())
    return true;

  // CC token at top-level is parsed as an expression. If the only element
  // body of the TopLevelCodeDecl is a CodeCompletionExpr without a base
  // expression, the user might be writing a top-level declaration.
  if (const TopLevelCodeDecl *TLCD = dyn_cast<const TopLevelCodeDecl>(DC)) {
    auto body = TLCD->getBody();
    if (!body || body->empty())
      return true;
    if (body->getElements().size() > 1)
      return false;
    auto expr = body->getFirstElement().dyn_cast<Expr *>();
    if (!expr)
      return false;
    if (CodeCompletionExpr *CCExpr = dyn_cast<CodeCompletionExpr>(expr)) {
      if (CCExpr->getBase() == nullptr)
        return true;
    }
  }

  return false;
}

/// Returns \c true if the completion is happening in local context such as
/// inside function bodies. i.e.:
///
///     if condition {
///       #true#
///     }
///     expr.#true#
///
///     #false#
///
///     struct S {
///       #false#
///       func foo() {
///         #true#
///       }
///     }
static bool isCompletionDeclContextLocalContext(DeclContext *DC) {
  if (!DC->isLocalContext())
    return false;
  if (isCodeCompletionAtTopLevel(DC))
    return false;
  return true;
}

/// Return \c true if the completion happens at top-level of a library file.
static bool isCodeCompletionAtTopLevelOfLibraryFile(const DeclContext *DC) {
  if (DC->getParentSourceFile()->isScriptMode())
    return false;
  return isCodeCompletionAtTopLevel(DC);
}

/// Build completions by doing visible decl lookup from a context.
class CompletionLookup final : public swift::VisibleDeclConsumer {
  CodeCompletionResultSink &Sink;
  ASTContext &Ctx;
  const DeclContext *CurrDeclContext;
  ModuleDecl *CurrModule;
  ClangImporter *Importer;
  CodeCompletionContext *CompletionContext;

  enum class LookupKind {
    ValueExpr,
    ValueInDeclContext,
    EnumElement,
    Type,
    TypeInDeclContext,
    ImportFromModule,
    GenericRequirement,
  };

  LookupKind Kind;

  /// Type of the user-provided expression for LookupKind::ValueExpr
  /// completions.
  Type ExprType;

  /// Whether the expr is of statically inferred metatype.
  bool IsStaticMetatype = false;

  /// User-provided base type for LookupKind::Type completions.
  Type BaseType;

  /// Expected types of the code completion expression.
  ExpectedTypeContext expectedTypeContext;

  bool CanCurrDeclContextHandleAsync = false;
  bool HaveDot = false;
  bool IsUnwrappedOptional = false;
  SourceLoc DotLoc;
  bool NeedLeadingDot = false;

  bool NeedOptionalUnwrap = false;
  unsigned NumBytesToEraseForOptionalUnwrap = 0;

  bool HaveLParen = false;
  bool IsSuperRefExpr = false;
  bool IsSelfRefExpr = false;
  bool IsKeyPathExpr = false;
  bool IsSwiftKeyPathExpr = false;
  bool IsAfterSwiftKeyPathRoot = false;
  bool IsDynamicLookup = false;
  bool IsCrossActorReference = false;
  bool PreferFunctionReferencesToCalls = false;
  bool HaveLeadingSpace = false;

  bool CheckForDuplicates = false;
  llvm::DenseSet<std::pair<const Decl *, Type>> PreviouslySeen;

  bool IncludeInstanceMembers = false;

  /// True if we are code completing inside a static method.
  bool InsideStaticMethod = false;

  /// Innermost method that the code completion point is in.
  const AbstractFunctionDecl *CurrentMethod = nullptr;

  Optional<SemanticContextKind> ForcedSemanticContext = None;
  bool IsUnresolvedMember = false;

public:
  bool FoundFunctionCalls = false;
  bool FoundFunctionsWithoutFirstKeyword = false;

private:
  bool isForCaching() const {
    return Kind == LookupKind::ImportFromModule;
  }

  void foundFunction(const AbstractFunctionDecl *AFD) {
    FoundFunctionCalls = true;
    const DeclName Name = AFD->getName();
    auto ArgNames = Name.getArgumentNames();
    if (ArgNames.empty())
      return;
    if (ArgNames[0].empty())
      FoundFunctionsWithoutFirstKeyword = true;
  }

  void foundFunction(const AnyFunctionType *AFT) {
    FoundFunctionCalls = true;
    auto Params = AFT->getParams();
    if (Params.empty())
      return;
    if (Params.size() == 1 && !Params[0].hasLabel()) {
      FoundFunctionsWithoutFirstKeyword = true;
      return;
    }
    if (!Params[0].hasLabel())
      FoundFunctionsWithoutFirstKeyword = true;
  }

  void setClangDeclKeywords(const ValueDecl *VD, CommandWordsPairs &Pairs,
                            CodeCompletionResultBuilder &Builder) {
    if (auto *CD = VD->getClangDecl()) {
      clang::comments::getClangDocKeyword(*Importer, CD, Pairs);
    } else {
      swift::markup::getSwiftDocKeyword(VD, Pairs);
    }
    Builder.addDeclDocCommentWords(llvm::makeArrayRef(Pairs));
  }

  /// Returns \c true if \p TAD is usable as a first type of a requirement in
  /// \c where clause for a context.
  /// \p selfTy must be a \c Self type of the context.
  static bool canBeUsedAsRequirementFirstType(Type selfTy, TypeAliasDecl *TAD) {
    auto T = TAD->getDeclaredInterfaceType();
    auto subMap = selfTy->getMemberSubstitutionMap(TAD->getParentModule(), TAD);
    T = T.subst(subMap)->getCanonicalType();

    ArchetypeType *archeTy = T->getAs<ArchetypeType>();
    if (!archeTy)
      return false;
    archeTy = archeTy->getRoot();

    // For protocol, the 'archeTy' should match with the 'baseTy' which is the
    // dynamic 'Self' type of the protocol. For nominal decls, 'archTy' should
    // be one of the generic params in 'selfTy'. Search 'archeTy' in 'baseTy'.
    return selfTy.findIf([&](Type T) { return archeTy->isEqual(T); });
  }

public:
  struct RequestedResultsTy {
    const ModuleDecl *TheModule;
    bool OnlyTypes;
    bool OnlyPrecedenceGroups;
    bool NeedLeadingDot;
    bool IncludeModuleQualifier;

    static RequestedResultsTy fromModule(const ModuleDecl *TheModule) {
      return { TheModule, false, false, false, true };
    }

    RequestedResultsTy onlyTypes() const {
      return { TheModule, true, false, NeedLeadingDot, IncludeModuleQualifier };
    }

    RequestedResultsTy onlyPrecedenceGroups() const {
      assert(!OnlyTypes && "onlyTypes() already includes precedence groups");
      return { TheModule, false, true, false, true };
    }

    RequestedResultsTy needLeadingDot(bool NeedDot) const {
      return {
          TheModule, OnlyTypes, OnlyPrecedenceGroups, NeedDot,
          IncludeModuleQualifier
      };
    }

    RequestedResultsTy withModuleQualifier(bool IncludeModule) const {
        return {
            TheModule, OnlyTypes, OnlyPrecedenceGroups, NeedLeadingDot,
            IncludeModule
        };
    }

    static RequestedResultsTy toplevelResults() {
      return { nullptr, false, false, false, true };
    }
  };

  std::vector<RequestedResultsTy> RequestedCachedResults;

public:
  CompletionLookup(CodeCompletionResultSink &Sink,
                   ASTContext &Ctx,
                   const DeclContext *CurrDeclContext,
                   CodeCompletionContext *CompletionContext = nullptr)
      : Sink(Sink), Ctx(Ctx), CurrDeclContext(CurrDeclContext),
        CurrModule(CurrDeclContext ? CurrDeclContext->getParentModule()
                                   : nullptr),
        Importer(static_cast<ClangImporter *>(CurrDeclContext->getASTContext().
          getClangModuleLoader())),
        CompletionContext(CompletionContext) {
    // Determine if we are doing code completion inside a static method.
    if (CurrDeclContext) {
      CurrentMethod = CurrDeclContext->getInnermostMethodContext();
      if (auto *FD = dyn_cast_or_null<FuncDecl>(CurrentMethod))
        InsideStaticMethod = FD->isStatic();
      CanCurrDeclContextHandleAsync = canDeclContextHandleAsync(CurrDeclContext);
    }
  }

  void setHaveDot(SourceLoc DotLoc) {
    HaveDot = true;
    this->DotLoc = DotLoc;
  }

  void setIsUnwrappedOptional(bool value) {
    IsUnwrappedOptional = value;
  }

  void setIsStaticMetatype(bool value) {
    IsStaticMetatype = value;
  }

  void setExpectedTypes(ArrayRef<Type> Types,
                        bool isImplicitSingleExpressionReturn,
                        bool preferNonVoid = false) {
    expectedTypeContext.isImplicitSingleExpressionReturn =
        isImplicitSingleExpressionReturn;
    expectedTypeContext.preferNonVoid = preferNonVoid;
    expectedTypeContext.possibleTypes.clear();
    expectedTypeContext.possibleTypes.reserve(Types.size());
    for (auto T : Types)
      if (T)
        expectedTypeContext.possibleTypes.push_back(T);
  }

  void setIdealExpectedType(Type Ty) {
    expectedTypeContext.idealType = Ty;
  }

  CodeCompletionContext::TypeContextKind typeContextKind() const {
    if (expectedTypeContext.empty() && !expectedTypeContext.preferNonVoid) {
      return CodeCompletionContext::TypeContextKind::None;
    } else if (expectedTypeContext.isImplicitSingleExpressionReturn) {
      return CodeCompletionContext::TypeContextKind::SingleExpressionBody;
    } else {
      return CodeCompletionContext::TypeContextKind::Required;
    }
  }

  bool needDot() const {
    return NeedLeadingDot;
  }

  void setHaveLParen(bool Value) {
    HaveLParen = Value;
  }

  void setIsSuperRefExpr(bool Value = true) {
    IsSuperRefExpr = Value;
  }

  void setIsSelfRefExpr(bool value) { IsSelfRefExpr = value; }

  void setIsKeyPathExpr() {
    IsKeyPathExpr = true;
  }

  void shouldCheckForDuplicates(bool value = true) {
    CheckForDuplicates = value;
  }

  void setIsSwiftKeyPathExpr(bool onRoot) {
    IsSwiftKeyPathExpr = true;
    IsAfterSwiftKeyPathRoot = onRoot;
  }

  void setIsDynamicLookup() {
    IsDynamicLookup = true;
  }

  void setPreferFunctionReferencesToCalls() {
    PreferFunctionReferencesToCalls = true;
  }

  void setHaveLeadingSpace(bool value) { HaveLeadingSpace = value; }

  void includeInstanceMembers() {
    IncludeInstanceMembers = true;
  }

  bool isHiddenModuleName(Identifier Name) {
    return (Name.str().startswith("_") ||
            Name == Ctx.SwiftShimsModuleName ||
            Name.str() == SWIFT_ONONE_SUPPORT);
  }

  void addSubModuleNames(std::vector<std::pair<std::string, bool>>
      &SubModuleNameVisibilityPairs) {
    for (auto &Pair : SubModuleNameVisibilityPairs) {
      CodeCompletionResultBuilder Builder(Sink,
                                          CodeCompletionResult::ResultKind::
                                          Declaration,
                                          SemanticContextKind::None,
                                          expectedTypeContext);
      auto MD = ModuleDecl::create(Ctx.getIdentifier(Pair.first), Ctx);
      Builder.setAssociatedDecl(MD);
      Builder.addBaseName(MD->getNameStr());
      Builder.addTypeAnnotation("Module");
      if (Pair.second)
        Builder.setNotRecommended(NotRecommendedReason::RedundantImport);
    }
  }

  void collectImportedModules(llvm::StringSet<> &ImportedModules) {
    SmallVector<ImportedModule, 16> Imported;
    SmallVector<ImportedModule, 16> FurtherImported;
    CurrDeclContext->getParentSourceFile()->getImportedModules(
        Imported,
        {ModuleDecl::ImportFilterKind::Exported,
         ModuleDecl::ImportFilterKind::Default,
         ModuleDecl::ImportFilterKind::ImplementationOnly});
    while (!Imported.empty()) {
      ModuleDecl *MD = Imported.back().importedModule;
      Imported.pop_back();
      if (!ImportedModules.insert(MD->getNameStr()).second)
        continue;
      FurtherImported.clear();
      MD->getImportedModules(FurtherImported,
                             ModuleDecl::ImportFilterKind::Exported);
      Imported.append(FurtherImported.begin(), FurtherImported.end());
    }
  }

  void addModuleName(ModuleDecl *MD, Optional<NotRecommendedReason> R = None) {

    // Don't add underscored cross-import overlay modules.
    if (MD->getDeclaringModuleIfCrossImportOverlay())
      return;

    CodeCompletionResultBuilder Builder(
        Sink,
        CodeCompletionResult::ResultKind::Declaration,
        SemanticContextKind::None,
        expectedTypeContext);
    Builder.setAssociatedDecl(MD);
    Builder.addBaseName(MD->getNameStr());
    Builder.addTypeAnnotation("Module");
    if (R)
      Builder.setNotRecommended(*R);
  }

  void addImportModuleNames() {
    SmallVector<Identifier, 0> ModuleNames;
    Ctx.getVisibleTopLevelModuleNames(ModuleNames);

    llvm::StringSet<> ImportedModules;
    collectImportedModules(ImportedModules);

    auto mainModuleName = CurrModule->getName();
    for (auto ModuleName : ModuleNames) {
      if (ModuleName == mainModuleName || isHiddenModuleName(ModuleName))
        continue;

      auto MD = ModuleDecl::create(ModuleName, Ctx);
      Optional<NotRecommendedReason> Reason = None;

      // Imported modules are not recommended.
      if (ImportedModules.count(MD->getNameStr()) != 0)
        Reason = NotRecommendedReason::RedundantImport;

      addModuleName(MD, Reason);
    }
  }

  SemanticContextKind getSemanticContext(const Decl *D,
                                         DeclVisibilityKind Reason,
                                         DynamicLookupInfo dynamicLookupInfo) {
    if (ForcedSemanticContext)
      return *ForcedSemanticContext;

    switch (Reason) {
    case DeclVisibilityKind::LocalVariable:
    case DeclVisibilityKind::FunctionParameter:
    case DeclVisibilityKind::GenericParameter:
      return SemanticContextKind::Local;

    case DeclVisibilityKind::MemberOfCurrentNominal:
      return SemanticContextKind::CurrentNominal;

    case DeclVisibilityKind::MemberOfProtocolConformedToByCurrentNominal:
    case DeclVisibilityKind::MemberOfSuper:
      return SemanticContextKind::Super;

    case DeclVisibilityKind::MemberOfOutsideNominal:
      return SemanticContextKind::OutsideNominal;

    case DeclVisibilityKind::VisibleAtTopLevel:
      if (CurrDeclContext && D->getModuleContext() == CurrModule) {
        // Treat global variables from the same source file as local when
        // completing at top-level.
        if (isa<VarDecl>(D) && isTopLevelSubcontext(CurrDeclContext) &&
            D->getDeclContext()->getParentSourceFile() ==
                CurrDeclContext->getParentSourceFile()) {
          return SemanticContextKind::Local;
        } else {
          return SemanticContextKind::CurrentModule;
        }
      } else {
        return SemanticContextKind::OtherModule;
      }

    case DeclVisibilityKind::DynamicLookup:
      switch (dynamicLookupInfo.getKind()) {
      case DynamicLookupInfo::None:
        llvm_unreachable("invalid DynamicLookupInfo::Kind for dynamic lookup");

      case DynamicLookupInfo::AnyObject:
        // AnyObject results can come from different modules, including the
        // current module, but we always assign them the OtherModule semantic
        // context.  These declarations are uniqued by signature, so it is
        // totally random (determined by the hash function) which of the
        // equivalent declarations (across multiple modules) we will get.
        return SemanticContextKind::OtherModule;

      case DynamicLookupInfo::KeyPathDynamicMember:
        // Use the visibility of the underlying declaration.
        // FIXME: KeyPath<AnyObject, U> !?!?
        assert(dynamicLookupInfo.getKeyPathDynamicMember().originalVisibility !=
               DeclVisibilityKind::DynamicLookup);
        return getSemanticContext(
            D, dynamicLookupInfo.getKeyPathDynamicMember().originalVisibility,
            {});
      }

    case DeclVisibilityKind::MemberOfProtocolDerivedByCurrentNominal:
      llvm_unreachable("should not see this kind");
    }
    llvm_unreachable("unhandled kind");
  }

  bool isUnresolvedMemberIdealType(Type Ty) {
    assert(Ty);
    if (!IsUnresolvedMember)
      return false;
    Type idealTy = expectedTypeContext.idealType;
    if (!idealTy)
      return false;
    /// Consider optional object type is the ideal.
    /// For exmaple:
    ///   enum MyEnum { case foo, bar }
    ///   func foo(_: MyEnum?)
    ///   fooo(.<HERE>)
    /// Prefer '.foo' and '.bar' over '.some' and '.none'.
    idealTy = idealTy->lookThroughAllOptionalTypes();
    return idealTy->isEqual(Ty);
  }

  void addValueBaseName(CodeCompletionResultBuilder &Builder,
                        DeclBaseName Name) {
    auto NameStr = Name.userFacingName();
    bool shouldEscapeKeywords;
    if (Name.isSpecial()) {
      // Special names (i.e. 'init') are always displayed as its user facing
      // name.
      shouldEscapeKeywords = false;
    } else if (ExprType) {
      // After dot. User can write any keyword after '.' except for `init` and
      // `self`. E.g. 'func `init`()' must be called by 'expr.`init`()'.
      shouldEscapeKeywords = NameStr == "self" || NameStr == "init";
    } else {
      // As primary expresson. We have to escape almost every keywords except
      // for 'self' and 'Self'.
      shouldEscapeKeywords = NameStr != "self" && NameStr != "Self";
    }

    if (!shouldEscapeKeywords) {
      Builder.addBaseName(NameStr);
    } else {
      SmallString<16> buffer;
      Builder.addBaseName(Builder.escapeKeyword(NameStr, true, buffer));
    }
  }

  void addLeadingDot(CodeCompletionResultBuilder &Builder) {
    if (NeedOptionalUnwrap) {
      Builder.setNumBytesToErase(NumBytesToEraseForOptionalUnwrap);
      Builder.addQuestionMark();
      Builder.addLeadingDot();
      return;
    }
    if (needDot())
      Builder.addLeadingDot();
  }

  void addTypeAnnotation(CodeCompletionResultBuilder &Builder, Type T,
                         GenericSignature genericSig = GenericSignature()) {
    PrintOptions PO;
    PO.OpaqueReturnTypePrinting =
        PrintOptions::OpaqueReturnTypePrintingMode::WithoutOpaqueKeyword;
    if (auto typeContext = CurrDeclContext->getInnermostTypeContext())
      PO.setBaseType(typeContext->getDeclaredTypeInContext());
    Builder.addTypeAnnotation(eraseArchetypes(T, genericSig), PO);
    Builder.setExpectedTypeRelation(
        calculateMaxTypeRelation(T, expectedTypeContext, CurrDeclContext));
  }

  void addTypeAnnotationForImplicitlyUnwrappedOptional(
      CodeCompletionResultBuilder &Builder, Type T,
      GenericSignature genericSig = GenericSignature(),
      bool dynamicOrOptional = false) {

    std::string suffix;
    // FIXME: This retains previous behavior, but in reality the type of dynamic
    // lookups is IUO, not Optional as it is for the @optional attribute.
    if (dynamicOrOptional) {
      T = T->getOptionalObjectType();
      suffix = "?";
    }

    PrintOptions PO;
    PO.PrintOptionalAsImplicitlyUnwrapped = true;
    PO.OpaqueReturnTypePrinting =
        PrintOptions::OpaqueReturnTypePrintingMode::WithoutOpaqueKeyword;
    if (auto typeContext = CurrDeclContext->getInnermostTypeContext())
      PO.setBaseType(typeContext->getDeclaredTypeInContext());
    Builder.addTypeAnnotation(eraseArchetypes(T, genericSig), PO, suffix);
    Builder.setExpectedTypeRelation(
        calculateMaxTypeRelation(T, expectedTypeContext, CurrDeclContext));
  }

  /// For printing in code completion results, replace archetypes with
  /// protocol compositions.
  ///
  /// FIXME: Perhaps this should be an option in PrintOptions instead.
  Type eraseArchetypes(Type type, GenericSignature genericSig) {
    if (!genericSig)
      return type;

    auto buildProtocolComposition = [&](ArrayRef<ProtocolDecl *> protos) -> Type {
      SmallVector<Type, 2> types;
      for (auto proto : protos)
        types.push_back(proto->getDeclaredInterfaceType());
      return ProtocolCompositionType::get(Ctx, types,
                                          /*HasExplicitAnyObject=*/false);
    };

    if (auto *genericFuncType = type->getAs<GenericFunctionType>()) {
      SmallVector<AnyFunctionType::Param, 8> erasedParams;
      for (const auto &param : genericFuncType->getParams()) {
        auto erasedTy = eraseArchetypes(param.getPlainType(), genericSig);
        erasedParams.emplace_back(param.withType(erasedTy));
      }
      return GenericFunctionType::get(genericSig,
          erasedParams,
          eraseArchetypes(genericFuncType->getResult(), genericSig),
          genericFuncType->getExtInfo());
    }

    return type.transform([&](Type t) -> Type {
      // FIXME: Code completion should only deal with one or the other,
      // and not both.
      if (auto *archetypeType = t->getAs<ArchetypeType>()) {
        // Don't erase opaque archetype.
        if (isa<OpaqueTypeArchetypeType>(archetypeType))
          return t;

        auto protos = archetypeType->getConformsTo();
        if (!protos.empty())
          return buildProtocolComposition(protos);
      }

      if (t->isTypeParameter()) {
        const auto protos = genericSig->getRequiredProtocols(t);
        if (!protos.empty())
          return buildProtocolComposition(protos);
      }

      return t;
    });
  }

  Type getTypeOfMember(const ValueDecl *VD,
                       DynamicLookupInfo dynamicLookupInfo) {
    switch (dynamicLookupInfo.getKind()) {
    case DynamicLookupInfo::None:
      return getTypeOfMember(VD, this->ExprType);
    case DynamicLookupInfo::AnyObject:
      return getTypeOfMember(VD, Type());
    case DynamicLookupInfo::KeyPathDynamicMember: {
      auto &keyPathInfo = dynamicLookupInfo.getKeyPathDynamicMember();

      // Map the result of VD to keypath member lookup results.
      // Given:
      //   struct Wrapper<T> {
      //     subscript<U>(dynamicMember: KeyPath<T, U>) -> Wrapped<U> { get }
      //   }
      //   struct Circle {
      //     var center: Point { get }
      //     var radius: Length { get }
      //   }
      //
      // Consider 'Wrapper<Circle>.center'.
      //   'VD' is 'Circle.center' decl.
      //   'keyPathInfo.subscript' is 'Wrapper<T>.subscript' decl.
      //   'keyPathInfo.baseType' is 'Wrapper<Circle>' type.

      // FIXME: Handle nested keypath member lookup.
      // i.e. cases where 'ExprType' != 'keyPathInfo.baseType'.

      auto *SD = keyPathInfo.subscript;
      const auto elementTy = SD->getElementInterfaceType();
      if (!elementTy->hasTypeParameter())
        return elementTy;

      // Map is:
      //   { τ_0_0(T) => Circle
      //     τ_1_0(U) => U }
      auto subs = keyPathInfo.baseType->getMemberSubstitutions(SD);

      // If the keyPath result type has type parameters, that might affect the
      // subscript result type.
      auto keyPathResultTy = getResultTypeOfKeypathDynamicMember(SD)->
        mapTypeOutOfContext();
      if (keyPathResultTy->hasTypeParameter()) {
        auto keyPathRootTy = getRootTypeOfKeypathDynamicMember(SD).
          subst(QueryTypeSubstitutionMap{subs},
                LookUpConformanceInModule(CurrModule));

        // The result type of the VD.
        // i.e. 'Circle.center' => 'Point'.
        auto innerResultTy = getTypeOfMember(VD, keyPathRootTy);

        if (auto paramTy = keyPathResultTy->getAs<GenericTypeParamType>()) {
          // Replace keyPath result type in the map with the inner result type.
          // i.e. Make the map as:
          //   { τ_0_0(T) => Circle
          //     τ_1_0(U) => Point }
          auto key =
              paramTy->getCanonicalType()->castTo<GenericTypeParamType>();
          subs[key] = innerResultTy;
        } else {
          // FIXME: Handle the case where the KeyPath result is generic.
          // e.g. 'subscript<U>(dynamicMember: KeyPath<T, Box<U>>) -> Bag<U>'
          // For now, just return the inner type.
          return innerResultTy;
        }
      }

      // Substitute the element type of the subscript using modified map.
      // i.e. 'Wrapped<U>' => 'Wrapped<Point>'.
      return elementTy.subst(QueryTypeSubstitutionMap{subs},
                             LookUpConformanceInModule(CurrModule));
    }
    }
    llvm_unreachable("Unhandled DynamicLookupInfo Kind in switch");
  }

  Type getTypeOfMember(const ValueDecl *VD, Type ExprType) {
    Type T = VD->getInterfaceType();
    assert(!T.isNull());

    if (ExprType) {
      Type ContextTy = VD->getDeclContext()->getDeclaredInterfaceType();
      if (ContextTy) {
        // Look through lvalue types and metatypes
        Type MaybeNominalType = ExprType->getRValueType();

        if (auto Metatype = MaybeNominalType->getAs<MetatypeType>())
          MaybeNominalType = Metatype->getInstanceType();

        if (auto SelfType = MaybeNominalType->getAs<DynamicSelfType>())
          MaybeNominalType = SelfType->getSelfType();

        // For optional protocol requirements and dynamic dispatch,
        // strip off optionality from the base type, but only if
        // we're not actually completing a member of Optional.
        if (!ContextTy->getOptionalObjectType() &&
            MaybeNominalType->getOptionalObjectType())
          MaybeNominalType = MaybeNominalType->getOptionalObjectType();

        // For dynamic lookup don't substitute in the base type.
        if (MaybeNominalType->isAnyObject())
          return T;

        // FIXME: Sometimes ExprType is the type of the member here,
        // and not the type of the base. That is inconsistent and
        // should be cleaned up.
        if (!MaybeNominalType->mayHaveMembers())
          return T;

        // We can't do anything if the base type has unbound generic parameters.
        if (MaybeNominalType->hasUnboundGenericType())
          return T;

        // For everything else, substitute in the base type.
        auto Subs = MaybeNominalType->getMemberSubstitutionMap(CurrModule, VD);

        // Pass in DesugarMemberTypes so that we see the actual
        // concrete type witnesses instead of type alias types.
        T = T.subst(Subs, SubstFlags::DesugarMemberTypes);
      }
    }

    return T;
  }

  Type getAssociatedTypeType(const AssociatedTypeDecl *ATD) {
    Type BaseTy = BaseType;
    if (!BaseTy)
      BaseTy = ExprType;
    if (!BaseTy && CurrDeclContext)
      BaseTy = CurrDeclContext->getInnermostTypeContext()
                   ->getDeclaredTypeInContext();
    if (BaseTy) {
      BaseTy = BaseTy->getInOutObjectType()->getMetatypeInstanceType();
      if (auto NTD = BaseTy->getAnyNominal()) {
        auto *Module = NTD->getParentModule();
        auto Conformance = Module->lookupConformance(
            BaseTy, ATD->getProtocol());
        if (Conformance.isConcrete()) {
          return Conformance.getConcrete()->getTypeWitness(
              const_cast<AssociatedTypeDecl *>(ATD));
        }
      }
    }
    return Type();
  }

  void analyzeActorIsolation(const ValueDecl *VD, Type T, bool &implicitlyAsync,
                             Optional<NotRecommendedReason> &NotRecommended) {
    auto isolation = getActorIsolation(const_cast<ValueDecl *>(VD));

    switch (isolation.getKind()) {
    case ActorIsolation::DistributedActorInstance: {
      // TODO: implicitlyThrowing here for distributed
      LLVM_FALLTHROUGH; // continue the ActorInstance checks
    }
    case ActorIsolation::ActorInstance: {
      if (IsCrossActorReference) {
        implicitlyAsync = true;
        // TODO: 'NotRecommended' if this is a r-value reference.
      }
      break;
    }
    case ActorIsolation::GlobalActor:
    case ActorIsolation::GlobalActorUnsafe: {
      auto contextIsolation = getActorIsolationOfContext(
          const_cast<DeclContext *>(CurrDeclContext));
      if (contextIsolation != isolation) {
        implicitlyAsync = true;
      }
      break;
    }
    case ActorIsolation::Unspecified:
    case ActorIsolation::Independent:
      return;
    }

    // If the reference is 'async', all types must be 'Sendable'.
    if (implicitlyAsync && T) {
      auto *M = CurrDeclContext->getParentModule();
      if (isa<VarDecl>(VD)) {
        if (!isSendableType(M, T)) {
          NotRecommended = NotRecommendedReason::CrossActorReference;
        }
      } else {
        assert(isa<FuncDecl>(VD) || isa<SubscriptDecl>(VD));
        // Check if the result and the param types are all 'Sendable'.
        auto *AFT = T->castTo<AnyFunctionType>();
        if (!isSendableType(M, AFT->getResult())) {
          NotRecommended = NotRecommendedReason::CrossActorReference;
        } else {
          for (auto &param : AFT->getParams()) {
            Type paramType = param.getPlainType();
            if (!isSendableType(M, paramType)) {
              NotRecommended = NotRecommendedReason::CrossActorReference;
              break;
            }
          }
        }
      }
    }
  }

  void addVarDeclRef(const VarDecl *VD, DeclVisibilityKind Reason,
                     DynamicLookupInfo dynamicLookupInfo) {
    if (!VD->hasName())
      return;

    const Identifier Name = VD->getName();
    assert(!Name.empty() && "name should not be empty");

    Type VarType;
    if (VD->hasInterfaceType())
      VarType = getTypeOfMember(VD, dynamicLookupInfo);

    Optional<NotRecommendedReason> NotRecommended;
    // "not recommended" in its own getter.
    if (Kind == LookupKind::ValueInDeclContext) {
      if (auto accessor = dyn_cast<AccessorDecl>(CurrDeclContext)) {
        if (accessor->getStorage() == VD && accessor->isGetter())
          NotRecommended = NotRecommendedReason::VariableUsedInOwnDefinition;
      }
    }
    bool implicitlyAsync = false;
    analyzeActorIsolation(VD, VarType, implicitlyAsync, NotRecommended);
    if (!isForCaching() && !NotRecommended && implicitlyAsync &&
        !CanCurrDeclContextHandleAsync) {
      NotRecommended = NotRecommendedReason::InvalidAsyncContext;
    }

    CommandWordsPairs Pairs;
    CodeCompletionResultBuilder Builder(
        Sink, CodeCompletionResult::ResultKind::Declaration,
        getSemanticContext(VD, Reason, dynamicLookupInfo), expectedTypeContext);
    Builder.setAssociatedDecl(VD);
    addLeadingDot(Builder);
    addValueBaseName(Builder, Name);
    setClangDeclKeywords(VD, Pairs, Builder);

    if (NotRecommended)
      Builder.setNotRecommended(*NotRecommended);

    if (!VarType)
      return;

    if (auto *PD = dyn_cast<ParamDecl>(VD)) {
      if (Name != Ctx.Id_self && PD->isInOut()) {
        // It is useful to show inout for function parameters.
        // But for 'self' it is just noise.
        VarType = InOutType::get(VarType);
      }
    }
    auto DynamicOrOptional =
        IsDynamicLookup || VD->getAttrs().hasAttribute<OptionalAttr>();
    if (DynamicOrOptional) {
      // Values of properties that were found on a AnyObject have
      // Optional<T> type.  Same applies to optional members.
      VarType = OptionalType::get(VarType);
    }

    auto genericSig =
        VD->getInnermostDeclContext()->getGenericSignatureOfContext();
    if (VD->isImplicitlyUnwrappedOptional())
      addTypeAnnotationForImplicitlyUnwrappedOptional(
          Builder, VarType, genericSig, DynamicOrOptional);
    else
      addTypeAnnotation(Builder, VarType, genericSig);

    if (implicitlyAsync)
      Builder.addAnnotatedAsync();

    if (isUnresolvedMemberIdealType(VarType))
      Builder.addFlair(CodeCompletionFlairBit::ExpressionSpecific);
  }

  static bool hasInterestingDefaultValues(const AbstractFunctionDecl *func) {
    if (!func) return false;

    for (auto param : *func->getParameters()) {
      switch (param->getDefaultArgumentKind()) {
      case DefaultArgumentKind::Normal:
      case DefaultArgumentKind::StoredProperty:
      case DefaultArgumentKind::Inherited: // FIXME: include this?
        return true;
      default:
        break;
      }
    }
    return false;
  }

  /// Build argument patterns for calling. Returns \c true if any content was
  /// added to \p Builder. If \p declParams is non-empty, the size must match
  /// with \p typeParams.
  bool addCallArgumentPatterns(CodeCompletionResultBuilder &Builder,
                               ArrayRef<AnyFunctionType::Param> typeParams,
                               ArrayRef<const ParamDecl *> declParams,
                               GenericSignature genericSig,
                               bool includeDefaultArgs = true) {
    assert(declParams.empty() || typeParams.size() == declParams.size());

    bool modifiedBuilder = false;

    // Determine whether we should skip this argument because it is defaulted.
    auto shouldSkipArg = [&](const ParamDecl *PD) -> bool {
      switch (PD->getDefaultArgumentKind()) {
      case DefaultArgumentKind::None:
        return false;

      case DefaultArgumentKind::Normal:
      case DefaultArgumentKind::StoredProperty:
      case DefaultArgumentKind::Inherited:
      case DefaultArgumentKind::NilLiteral:
      case DefaultArgumentKind::EmptyArray:
      case DefaultArgumentKind::EmptyDictionary:
        return !includeDefaultArgs;

#define MAGIC_IDENTIFIER(NAME, STRING, SYNTAX_KIND) \
      case DefaultArgumentKind::NAME:
#include "swift/AST/MagicIdentifierKinds.def"
        // Skip parameters that are defaulted to source location or other
        // caller context information.  Users typically don't want to specify
        // these parameters.
        return true;
      }

      llvm_unreachable("Unhandled DefaultArgumentKind in switch.");
    };

    bool NeedComma = false;
    // Iterate over each parameter.
    for (unsigned i = 0; i != typeParams.size(); ++i) {
      auto &typeParam = typeParams[i];

      Identifier argName;
      Identifier bodyName;
      bool isIUO = false;

      if (!declParams.empty()) {
        auto *PD = declParams[i];
        if (shouldSkipArg(PD))
          continue;
        argName = PD->getArgumentName();
        bodyName = PD->getParameterName();
        isIUO = PD->isImplicitlyUnwrappedOptional();
      } else {
        isIUO = false;
        argName = typeParam.getLabel();
      }

      bool isVariadic = typeParam.isVariadic();
      bool isInOut = typeParam.isInOut();
      bool isAutoclosure = typeParam.isAutoClosure();
      Type paramTy = typeParam.getPlainType();
      if (isVariadic)
        paramTy = ParamDecl::getVarargBaseTy(paramTy);

      if (NeedComma)
        Builder.addComma();
      Type contextTy;
      if (auto typeContext = CurrDeclContext->getInnermostTypeContext())
        contextTy = typeContext->getDeclaredTypeInContext();

      Builder.addCallParameter(argName, bodyName,
                               eraseArchetypes(paramTy, genericSig), contextTy,
                               isVariadic, isInOut, isIUO, isAutoclosure,
                               /*useUnderscoreLabel=*/false,
                               /*isLabeledTrailingClosure=*/false);

      modifiedBuilder = true;
      NeedComma = true;
    }
    return modifiedBuilder;
  }

  /// Build argument patterns for calling. Returns \c true if any content was
  /// added to \p Builder. If \p Params is non-nullptr, \F
  bool addCallArgumentPatterns(CodeCompletionResultBuilder &Builder,
                               const AnyFunctionType *AFT,
                               const ParameterList *Params,
                               GenericSignature genericSig,
                               bool includeDefaultArgs = true) {
    ArrayRef<const ParamDecl *> declParams;
    if (Params)
      declParams = Params->getArray();
    return addCallArgumentPatterns(Builder, AFT->getParams(), declParams,
                                   genericSig, includeDefaultArgs);
  }

  static void addEffectsSpecifiers(CodeCompletionResultBuilder &Builder,
                             const AnyFunctionType *AFT,
                             const AbstractFunctionDecl *AFD,
                             bool forceAsync = false) {
    assert(AFT != nullptr);

    // 'async'.
    if (forceAsync || (AFD && AFD->hasAsync()) || AFT->isAsync())
      Builder.addAnnotatedAsync();

    // 'throws' or 'rethrows'.
    if (AFD && AFD->getAttrs().hasAttribute<RethrowsAttr>())
      Builder.addAnnotatedRethrows();
    else if (AFT->isThrowing())
      Builder.addAnnotatedThrows();
  }

  void addPoundAvailable(Optional<StmtKind> ParentKind) {
    if (ParentKind != StmtKind::If && ParentKind != StmtKind::Guard)
      return;
    CodeCompletionResultBuilder Builder(Sink, CodeCompletionResult::ResultKind::Keyword,
      // FIXME: SemanticContextKind::Local is not correct.
      // Use 'None' (and fix prioritization) or introduce a new context.
      SemanticContextKind::Local, expectedTypeContext);
    Builder.addFlair(CodeCompletionFlairBit::ExpressionSpecific);
    Builder.addBaseName("available");
    Builder.addLeftParen();
    Builder.addSimpleTypedParameter("Platform", /*IsVarArg=*/true);
    Builder.addComma();
    Builder.addTextChunk("*");
    Builder.addRightParen();
  }

  void addPoundSelector(bool needPound) {
    // #selector is only available when the Objective-C runtime is.
    if (!Ctx.LangOpts.EnableObjCInterop) return;

    CodeCompletionResultBuilder Builder(
        Sink, CodeCompletionResult::ResultKind::Keyword,
        SemanticContextKind::None, {});
    if (needPound)
      Builder.addTextChunk("#selector");
    else
      Builder.addTextChunk("selector");
    Builder.addLeftParen();
    Builder.addSimpleTypedParameter("@objc method", /*IsVarArg=*/false);
    Builder.addRightParen();
    Builder.addTypeAnnotation("Selector");
    // This function is called only if the context type is 'Selector'.
    Builder.setExpectedTypeRelation(
        CodeCompletionResult::ExpectedTypeRelation::Identical);
  }

  void addPoundKeyPath(bool needPound) {
    // #keyPath is only available when the Objective-C runtime is.
    if (!Ctx.LangOpts.EnableObjCInterop) return;

    CodeCompletionResultBuilder Builder(
        Sink, CodeCompletionResult::ResultKind::Keyword,
        SemanticContextKind::None, {});
    if (needPound)
      Builder.addTextChunk("#keyPath");
    else
      Builder.addTextChunk("keyPath");
    Builder.addLeftParen();
    Builder.addSimpleTypedParameter("@objc property sequence",
                                    /*IsVarArg=*/false);
    Builder.addRightParen();
    Builder.addTypeAnnotation("String");
    // This function is called only if the context type is 'String'.
    Builder.setExpectedTypeRelation(
        CodeCompletionResult::ExpectedTypeRelation::Identical);
  }

  SemanticContextKind getSemanticContextKind(const ValueDecl *VD) {
    // FIXME: to get the corect semantic context we need to know how lookup
    // would have found the VD. For now, just infer a reasonable semantics.

    if (!VD)
      return SemanticContextKind::CurrentModule;

    DeclContext *calleeDC = VD->getDeclContext();
    
    if (calleeDC->isTypeContext())
      // FIXME: We should distinguish CurrentNominal and Super. We need to
      // propagate the base type to do that.
      return SemanticContextKind::CurrentNominal;

    if (calleeDC->isLocalContext())
      return SemanticContextKind::Local;
    if (calleeDC->getParentModule() == CurrModule)
      return SemanticContextKind::CurrentModule;

    return SemanticContextKind::OtherModule;
  }

  void addSubscriptCallPattern(
      const AnyFunctionType *AFT, const SubscriptDecl *SD,
      const Optional<SemanticContextKind> SemanticContext = None) {
    foundFunction(AFT);
    GenericSignature genericSig;
    if (SD)
      genericSig = SD->getGenericSignatureOfContext();

    CommandWordsPairs Pairs;
    CodeCompletionResultBuilder Builder(
        Sink,
        SD ? CodeCompletionResult::ResultKind::Declaration
           : CodeCompletionResult::ResultKind::Pattern,
        SemanticContext ? *SemanticContext : getSemanticContextKind(SD),
        expectedTypeContext);
    if (SD) {
      Builder.setAssociatedDecl(SD);
      setClangDeclKeywords(SD, Pairs, Builder);
    }
    if (!HaveLParen) {
      Builder.addLeftBracket();
    } else {
      // Add 'ArgumentLabels' only if it has '['. Without existing '[',
      // consider it suggesting 'subscript' itself, not call arguments for it.
      Builder.addFlair(CodeCompletionFlairBit::ArgumentLabels);
      Builder.addAnnotatedLeftBracket();
    }
    ArrayRef<const ParamDecl *> declParams;
    if (SD)
      declParams = SD->getIndices()->getArray();
    addCallArgumentPatterns(Builder, AFT->getParams(), declParams, genericSig);
    if (!HaveLParen)
      Builder.addRightBracket();
    else
      Builder.addAnnotatedRightBracket();
    if (SD && SD->isImplicitlyUnwrappedOptional())
      addTypeAnnotationForImplicitlyUnwrappedOptional(Builder,
                                                      AFT->getResult(),
                                                      genericSig);
    else
      addTypeAnnotation(Builder, AFT->getResult(), genericSig);
  }

  void addFunctionCallPattern(
      const AnyFunctionType *AFT, const AbstractFunctionDecl *AFD = nullptr,
      const Optional<SemanticContextKind> SemanticContext = None) {
    GenericSignature genericSig;
    if (AFD)
      genericSig = AFD->getGenericSignatureOfContext();

    // Add the pattern, possibly including any default arguments.
    auto addPattern = [&](ArrayRef<const ParamDecl *> declParams = {},
                          bool includeDefaultArgs = true) {
      CommandWordsPairs Pairs;
      CodeCompletionResultBuilder Builder(
          Sink,
          AFD ? CodeCompletionResult::ResultKind::Declaration
              : CodeCompletionResult::ResultKind::Pattern,
          SemanticContext ? *SemanticContext : getSemanticContextKind(AFD),
          expectedTypeContext);
      Builder.addFlair(CodeCompletionFlairBit::ArgumentLabels);
      if (AFD) {
        Builder.setAssociatedDecl(AFD);
        setClangDeclKeywords(AFD, Pairs, Builder);
      }

      if (!HaveLParen)
        Builder.addLeftParen();
      else
        Builder.addAnnotatedLeftParen();

      addCallArgumentPatterns(Builder, AFT->getParams(), declParams,
                              genericSig, includeDefaultArgs);

      // The rparen matches the lparen here so that we insert both or neither.
      if (!HaveLParen)
        Builder.addRightParen();
      else
        Builder.addAnnotatedRightParen();

      addEffectsSpecifiers(Builder, AFT, AFD);

      if (AFD &&
          AFD->isImplicitlyUnwrappedOptional())
        addTypeAnnotationForImplicitlyUnwrappedOptional(Builder,
                                                        AFT->getResult(),
                                                        genericSig);
      else
        addTypeAnnotation(Builder, AFT->getResult(), genericSig);

      if (!isForCaching() && AFT->isAsync() && !CanCurrDeclContextHandleAsync) {
        Builder.setNotRecommended(NotRecommendedReason::InvalidAsyncContext);
      }
    };

    if (!AFD || !AFD->getInterfaceType()->is<AnyFunctionType>()) {
      // Probably, calling closure type expression.
      foundFunction(AFT);
      addPattern();
      return;
    } else {
      // Calling function or method.
      foundFunction(AFD);

      // FIXME: Hack because we don't know we are calling instance
      // method or not. There's invariant that funcTy is derived from AFD.
      // Only if we are calling instance method on meta type, AFT is still
      // curried. So we should be able to detect that by comparing curried level
      // of AFT and the interface type of AFD.
      auto getCurriedLevel = [](const AnyFunctionType *funcTy) -> unsigned {
        unsigned level = 0;
        while ((funcTy = funcTy->getResult()->getAs<AnyFunctionType>()))
          ++level;
        return level;
      };
      bool isImplicitlyCurriedInstanceMethod =
          (AFD->hasImplicitSelfDecl() &&
           getCurriedLevel(AFT) ==
               getCurriedLevel(
                   AFD->getInterfaceType()->castTo<AnyFunctionType>()) &&
           // NOTE: shouldn't be necessary, but just in case curried level check
           // is insufficient.
           AFT->getParams().size() == 1 &&
           AFT->getParams()[0].getLabel().empty());

      if (isImplicitlyCurriedInstanceMethod) {
        addPattern({AFD->getImplicitSelfDecl()}, /*includeDefaultArgs=*/true);
      } else {
        if (hasInterestingDefaultValues(AFD))
          addPattern(AFD->getParameters()->getArray(),
                     /*includeDefaultArgs=*/false);
        addPattern(AFD->getParameters()->getArray(),
                   /*includeDefaultArgs=*/true);
      }
    }
  }

  bool isImplicitlyCurriedInstanceMethod(const AbstractFunctionDecl *FD) {
    if (FD->isStatic())
      return false;

    switch (Kind) {
    case LookupKind::ValueExpr:
      return ExprType->is<AnyMetatypeType>();
    case LookupKind::ValueInDeclContext:
      if (InsideStaticMethod)
        return FD->getDeclContext() == CurrentMethod->getDeclContext();
      if (auto Init = dyn_cast<Initializer>(CurrDeclContext)) {
        if (auto PatInit = dyn_cast<PatternBindingInitializer>(Init)) {
          if (PatInit->getInitializedLazyVar())
            return false;
        }
        return FD->getDeclContext() == Init->getInnermostTypeContext();
      }
      return false;
    case LookupKind::EnumElement:
    case LookupKind::Type:
    case LookupKind::TypeInDeclContext:
    case LookupKind::GenericRequirement:
      llvm_unreachable("cannot have a method call while doing a "
                       "type completion");
    case LookupKind::ImportFromModule:
      return false;
    }

    llvm_unreachable("Unhandled LookupKind in switch.");
  }

  void addMethodCall(const FuncDecl *FD, DeclVisibilityKind Reason,
                     DynamicLookupInfo dynamicLookupInfo) {
    if (FD->getBaseIdentifier().empty())
      return;
    foundFunction(FD);

    const Identifier Name = FD->getBaseIdentifier();
    assert(!Name.empty() && "name should not be empty");

    Type FunctionType = getTypeOfMember(FD, dynamicLookupInfo);
    assert(FunctionType);

    auto AFT = FunctionType->getAs<AnyFunctionType>();

    bool IsImplicitlyCurriedInstanceMethod = false;
    if (FD->hasImplicitSelfDecl()) {
      IsImplicitlyCurriedInstanceMethod = isImplicitlyCurriedInstanceMethod(FD);

      // Strip off '(_ self: Self)' if needed.
      if (AFT && !IsImplicitlyCurriedInstanceMethod) {
        AFT = AFT->getResult()->getAs<AnyFunctionType>();

        // Check for duplicates with the adjusted type too.
        if (isDuplicate(FD, AFT))
          return;
      }
    }

    bool trivialTrailingClosure = false;
    if (AFT && !IsImplicitlyCurriedInstanceMethod)
      trivialTrailingClosure = hasTrivialTrailingClosure(FD, AFT);

    Optional<NotRecommendedReason> NotRecommended;
    bool implictlyAsync = false;
    analyzeActorIsolation(FD, AFT, implictlyAsync, NotRecommended);

    if (!isForCaching() && !NotRecommended &&
        !IsImplicitlyCurriedInstanceMethod &&
        ((AFT && AFT->isAsync()) || implictlyAsync) &&
        !CanCurrDeclContextHandleAsync) {
      NotRecommended = NotRecommendedReason::InvalidAsyncContext;
    }

    // Add the method, possibly including any default arguments.
    auto addMethodImpl = [&](bool includeDefaultArgs = true,
                             bool trivialTrailingClosure = false) {
      CommandWordsPairs Pairs;
      CodeCompletionResultBuilder Builder(
          Sink, CodeCompletionResult::ResultKind::Declaration,
          getSemanticContext(FD, Reason, dynamicLookupInfo),
          expectedTypeContext);
      setClangDeclKeywords(FD, Pairs, Builder);
      Builder.setAssociatedDecl(FD);

      if (IsSuperRefExpr && CurrentMethod &&
          CurrentMethod->getOverriddenDecl() == FD)
        Builder.addFlair(CodeCompletionFlairBit::SuperChain);

      if (NotRecommended)
        Builder.setNotRecommended(*NotRecommended);

      addLeadingDot(Builder);
      addValueBaseName(Builder, Name);
      if (IsDynamicLookup)
        Builder.addDynamicLookupMethodCallTail();
      else if (FD->getAttrs().hasAttribute<OptionalAttr>())
        Builder.addOptionalMethodCallTail();

      if (!AFT) {
        addTypeAnnotation(Builder, FunctionType,
                          FD->getGenericSignatureOfContext());
        return;
      }
      if (IsImplicitlyCurriedInstanceMethod) {
        Builder.addLeftParen();
        addCallArgumentPatterns(Builder, AFT->getParams(),
                                {FD->getImplicitSelfDecl()},
                                FD->getGenericSignatureOfContext(),
                                includeDefaultArgs);
        Builder.addRightParen();
      } else if (trivialTrailingClosure) {
        Builder.addBraceStmtWithCursor(" { code }");
        addEffectsSpecifiers(Builder, AFT, FD, implictlyAsync);
      } else {
        Builder.addLeftParen();
        addCallArgumentPatterns(Builder, AFT, FD->getParameters(),
                                FD->getGenericSignatureOfContext(),
                                includeDefaultArgs);
        Builder.addRightParen();
        addEffectsSpecifiers(Builder, AFT, FD, implictlyAsync);
      }

      // Build type annotation.
      Type ResultType = AFT->getResult();
      // As we did with parameters in addParamPatternFromFunction,
      // for regular methods we'll print '!' after implicitly
      // unwrapped optional results.
      bool IsIUO =
          !IsImplicitlyCurriedInstanceMethod &&
          FD->isImplicitlyUnwrappedOptional();

      PrintOptions PO;
      PO.OpaqueReturnTypePrinting =
          PrintOptions::OpaqueReturnTypePrintingMode::WithoutOpaqueKeyword;
      PO.PrintOptionalAsImplicitlyUnwrapped = IsIUO;
      if (auto typeContext = CurrDeclContext->getInnermostTypeContext())
        PO.setBaseType(typeContext->getDeclaredTypeInContext());
      Type AnnotationTy =
          eraseArchetypes(ResultType, FD->getGenericSignatureOfContext());
      if (Builder.shouldAnnotateResults()) {
        Builder.withNestedGroup(
            CodeCompletionString::Chunk::ChunkKind::TypeAnnotationBegin, [&] {
              AnnotatedTypePrinter printer(Builder);
              if (IsImplicitlyCurriedInstanceMethod) {
                auto *FnType = AnnotationTy->castTo<AnyFunctionType>();
                AnyFunctionType::printParams(FnType->getParams(), printer,
                                             PrintOptions());
                AnnotationTy = FnType->getResult();
                printer.printText(" -> ");
              }

              // What's left is the result type.
              if (AnnotationTy->isVoid())
                AnnotationTy = Ctx.getVoidDecl()->getDeclaredInterfaceType();
              AnnotationTy.print(printer, PO);
            });
      } else {
        llvm::SmallString<32> TypeStr;
        llvm::raw_svector_ostream OS(TypeStr);
        if (IsImplicitlyCurriedInstanceMethod) {
          auto *FnType = AnnotationTy->castTo<AnyFunctionType>();
          AnyFunctionType::printParams(FnType->getParams(), OS);
          AnnotationTy = FnType->getResult();
          OS << " -> ";
        }

        // What's left is the result type.
        if (AnnotationTy->isVoid())
          AnnotationTy = Ctx.getVoidDecl()->getDeclaredInterfaceType();
        AnnotationTy.print(OS, PO);
        Builder.addTypeAnnotation(TypeStr);
      }

      Builder.setExpectedTypeRelation(calculateMaxTypeRelation(
          ResultType, expectedTypeContext, CurrDeclContext));

      if (isUnresolvedMemberIdealType(ResultType))
        Builder.addFlair(CodeCompletionFlairBit::ExpressionSpecific);

      if (!IsImplicitlyCurriedInstanceMethod &&
          expectedTypeContext.requiresNonVoid() &&
          ResultType->isVoid()) {
        Builder.setExpectedTypeRelation(CodeCompletionResult::Invalid);
      }
    };

    if (!AFT || IsImplicitlyCurriedInstanceMethod) {
      addMethodImpl();
    } else {
      if (trivialTrailingClosure)
        addMethodImpl(/*includeDefaultArgs=*/false,
                      /*trivialTrailingClosure=*/true);
      if (hasInterestingDefaultValues(FD))
        addMethodImpl(/*includeDefaultArgs=*/false);
      addMethodImpl(/*includeDefaultArgs=*/true);
    }
  }

  void addConstructorCall(const ConstructorDecl *CD, DeclVisibilityKind Reason,
                          DynamicLookupInfo dynamicLookupInfo,
                          Optional<Type> BaseType, Optional<Type> Result,
                          bool IsOnType = true,
                          Identifier addName = Identifier()) {
    foundFunction(CD);
    Type MemberType = getTypeOfMember(CD, BaseType.getValueOr(ExprType));
    AnyFunctionType *ConstructorType = nullptr;
    if (auto MemberFuncType = MemberType->getAs<AnyFunctionType>())
      ConstructorType = MemberFuncType->getResult()
                                      ->castTo<AnyFunctionType>();

    bool needInit = false;
    if (!IsOnType) {
      assert(addName.empty());
      needInit = true;
    } else if (addName.empty() && HaveDot) {
      needInit = true;
    }

    // If we won't be able to provide a result, bail out.
    if (!ConstructorType && addName.empty() && !needInit)
      return;

    // Add the constructor, possibly including any default arguments.
    auto addConstructorImpl = [&](bool includeDefaultArgs = true) {
      CommandWordsPairs Pairs;
      CodeCompletionResultBuilder Builder(
          Sink, CodeCompletionResult::ResultKind::Declaration,
          getSemanticContext(CD, Reason, dynamicLookupInfo),
          expectedTypeContext);
      setClangDeclKeywords(CD, Pairs, Builder);
      Builder.setAssociatedDecl(CD);

      if (IsSuperRefExpr && CurrentMethod &&
          CurrentMethod->getOverriddenDecl() == CD)
        Builder.addFlair(CodeCompletionFlairBit::SuperChain);

      if (needInit) {
        assert(addName.empty());
        addLeadingDot(Builder);
        Builder.addBaseName("init");
      } else if (!addName.empty()) {
        Builder.addBaseName(addName.str());
      } else {
        Builder.addFlair(CodeCompletionFlairBit::ArgumentLabels);
      }

      if (!ConstructorType) {
        addTypeAnnotation(Builder, MemberType,
                          CD->getGenericSignatureOfContext());
        return;
      }

      if (!HaveLParen)
        Builder.addLeftParen();
      else
        Builder.addAnnotatedLeftParen();

      addCallArgumentPatterns(Builder, ConstructorType, CD->getParameters(),
                              CD->getGenericSignatureOfContext(),
                              includeDefaultArgs);

      // The rparen matches the lparen here so that we insert both or neither.
      if (!HaveLParen)
        Builder.addRightParen();
      else
        Builder.addAnnotatedRightParen();

      addEffectsSpecifiers(Builder, ConstructorType, CD);

      if (!Result.hasValue())
        Result = ConstructorType->getResult();
      if (CD->isImplicitlyUnwrappedOptional()) {
        addTypeAnnotationForImplicitlyUnwrappedOptional(
            Builder, *Result, CD->getGenericSignatureOfContext());
      } else {
        addTypeAnnotation(Builder, *Result, CD->getGenericSignatureOfContext());
      }

      if (!isForCaching() && ConstructorType->isAsync() &&
          !CanCurrDeclContextHandleAsync) {
        Builder.setNotRecommended(NotRecommendedReason::InvalidAsyncContext);
      }
    };

    if (ConstructorType && hasInterestingDefaultValues(CD))
      addConstructorImpl(/*includeDefaultArgs*/ false);
    addConstructorImpl();
  }

  void addConstructorCallsForType(Type type, Identifier name,
                                  DeclVisibilityKind Reason,
                                  DynamicLookupInfo dynamicLookupInfo) {
    if (!Ctx.LangOpts.CodeCompleteInitsInPostfixExpr)
      return;

    assert(CurrDeclContext);

    auto results =
        swift::lookupSemanticMember(const_cast<DeclContext *>(CurrDeclContext),
                                    type, DeclBaseName::createConstructor());
    for (const auto &entry : results.allResults()) {
      auto *init = cast<ConstructorDecl>(entry.getValueDecl());
      if (init->shouldHideFromEditor())
        continue;
      addConstructorCall(cast<ConstructorDecl>(init), Reason,
                         dynamicLookupInfo, type, None,
                         /*IsOnType=*/true, name);
    }
  }

  void addSubscriptCall(const SubscriptDecl *SD, DeclVisibilityKind Reason,
                        DynamicLookupInfo dynamicLookupInfo) {
    // Don't add subscript call to unqualified completion.
    if (!ExprType)
      return;

    // Subscript after '.' is valid only after type part of Swift keypath
    // expression. (e.g. '\TyName.SubTy.[0])
    if (HaveDot && !IsAfterSwiftKeyPathRoot)
      return;

    auto subscriptType =
        getTypeOfMember(SD, dynamicLookupInfo)->getAs<AnyFunctionType>();
    if (!subscriptType)
      return;

    Optional<NotRecommendedReason> NotRecommended;
    bool implictlyAsync = false;
    analyzeActorIsolation(SD, subscriptType, implictlyAsync, NotRecommended);

    if (!isForCaching() && !NotRecommended && implictlyAsync &&
        !CanCurrDeclContextHandleAsync) {
      NotRecommended = NotRecommendedReason::InvalidAsyncContext;
    }

    CommandWordsPairs Pairs;
    CodeCompletionResultBuilder Builder(
        Sink, CodeCompletionResult::ResultKind::Declaration,
        getSemanticContext(SD, Reason, dynamicLookupInfo), expectedTypeContext);
    Builder.setAssociatedDecl(SD);
    setClangDeclKeywords(SD, Pairs, Builder);

    if (NotRecommended)
      Builder.setNotRecommended(*NotRecommended);

    // '\TyName#^TOKEN^#' requires leading dot.
    if (!HaveDot && IsAfterSwiftKeyPathRoot)
      Builder.addLeadingDot();

    if (NeedOptionalUnwrap) {
      Builder.setNumBytesToErase(NumBytesToEraseForOptionalUnwrap);
      Builder.addQuestionMark();
    }

    Builder.addLeftBracket();
    addCallArgumentPatterns(Builder, subscriptType, SD->getIndices(),
                            SD->getGenericSignatureOfContext(), true);
    Builder.addRightBracket();

    // Add a type annotation.
    Type resultTy = subscriptType->getResult();
    if (IsDynamicLookup) {
      // Values of properties that were found on a AnyObject have
      // Optional<T> type.
      resultTy = OptionalType::get(resultTy);
    }

    if (implictlyAsync)
      Builder.addAnnotatedAsync();

    addTypeAnnotation(Builder, resultTy, SD->getGenericSignatureOfContext());
  }

  void addNominalTypeRef(const NominalTypeDecl *NTD, DeclVisibilityKind Reason,
                         DynamicLookupInfo dynamicLookupInfo) {
    CommandWordsPairs Pairs;
    CodeCompletionResultBuilder Builder(
        Sink, CodeCompletionResult::ResultKind::Declaration,
        getSemanticContext(NTD, Reason, dynamicLookupInfo),
        expectedTypeContext);
    Builder.setAssociatedDecl(NTD);
    setClangDeclKeywords(NTD, Pairs, Builder);
    addLeadingDot(Builder);
    Builder.addBaseName(NTD->getName().str());

    addTypeAnnotation(Builder, NTD->getDeclaredType());

    // Override the type relation for NominalTypes. Use the better relation
    // for the metatypes and the instance type. For example,
    //
    //   func receiveInstance(_: Int) {}
    //   func receiveMetatype(_: Int.Type) {}
    //
    // We want to suggest 'Int' as 'Identical' for both arguments.
    Builder.setExpectedTypeRelation(std::max(
        calculateMaxTypeRelation(NTD->getDeclaredInterfaceType(),
                                 expectedTypeContext, CurrDeclContext),
        calculateMaxTypeRelation(NTD->getInterfaceType(), expectedTypeContext,
                                 CurrDeclContext)));
  }

  void addTypeAliasRef(const TypeAliasDecl *TAD, DeclVisibilityKind Reason,
                       DynamicLookupInfo dynamicLookupInfo) {
    CommandWordsPairs Pairs;
    CodeCompletionResultBuilder Builder(
        Sink, CodeCompletionResult::ResultKind::Declaration,
        getSemanticContext(TAD, Reason, dynamicLookupInfo),
        expectedTypeContext);
    Builder.setAssociatedDecl(TAD);
    setClangDeclKeywords(TAD, Pairs, Builder);
    addLeadingDot(Builder);
    Builder.addBaseName(TAD->getName().str());
    if (auto underlyingType = TAD->getUnderlyingType()) {
      if (underlyingType->hasError()) {
        Type parentType;
        if (auto nominal = TAD->getDeclContext()->getSelfNominalTypeDecl()) {
          parentType = nominal->getDeclaredInterfaceType();
        }
        addTypeAnnotation(
                      Builder,
                      TypeAliasType::get(const_cast<TypeAliasDecl *>(TAD),
                                         parentType, SubstitutionMap(),
                                         underlyingType));

      } else {
        addTypeAnnotation(Builder, underlyingType);
      }
    }
  }

  void addGenericTypeParamRef(const GenericTypeParamDecl *GP,
                              DeclVisibilityKind Reason,
                              DynamicLookupInfo dynamicLookupInfo) {
    CommandWordsPairs Pairs;
    CodeCompletionResultBuilder Builder(
        Sink, CodeCompletionResult::ResultKind::Declaration,
        getSemanticContext(GP, Reason, dynamicLookupInfo), expectedTypeContext);
    setClangDeclKeywords(GP, Pairs, Builder);
    Builder.setAssociatedDecl(GP);
    addLeadingDot(Builder);
    Builder.addBaseName(GP->getName().str());
    addTypeAnnotation(Builder, GP->getDeclaredInterfaceType());
  }

  void addAssociatedTypeRef(const AssociatedTypeDecl *AT,
                            DeclVisibilityKind Reason,
                            DynamicLookupInfo dynamicLookupInfo) {
    CommandWordsPairs Pairs;
    CodeCompletionResultBuilder Builder(
        Sink, CodeCompletionResult::ResultKind::Declaration,
        getSemanticContext(AT, Reason, dynamicLookupInfo), expectedTypeContext);
    setClangDeclKeywords(AT, Pairs, Builder);
    Builder.setAssociatedDecl(AT);
    addLeadingDot(Builder);
    Builder.addBaseName(AT->getName().str());
    if (Type T = getAssociatedTypeType(AT))
      addTypeAnnotation(Builder, T);
  }

  void addPrecedenceGroupRef(PrecedenceGroupDecl *PGD) {
    auto semanticContext =
        getSemanticContext(PGD, DeclVisibilityKind::VisibleAtTopLevel, {});
    CodeCompletionResultBuilder builder(
      Sink, CodeCompletionResult::ResultKind::Declaration,
      semanticContext, {});

    builder.addBaseName(PGD->getName().str());
    builder.setAssociatedDecl(PGD);
  };

  void addEnumElementRef(const EnumElementDecl *EED, DeclVisibilityKind Reason,
                         DynamicLookupInfo dynamicLookupInfo,
                         bool HasTypeContext) {
    if (!EED->hasName() ||
        !EED->isAccessibleFrom(CurrDeclContext) ||
        EED->shouldHideFromEditor())
      return;

    CommandWordsPairs Pairs;
    CodeCompletionResultBuilder Builder(
        Sink, CodeCompletionResult::ResultKind::Declaration,
        getSemanticContext(EED, Reason, dynamicLookupInfo),
        expectedTypeContext);
    Builder.setAssociatedDecl(EED);
    setClangDeclKeywords(EED, Pairs, Builder);
    if (HasTypeContext)
      Builder.addFlair(CodeCompletionFlairBit::ExpressionSpecific);

    addLeadingDot(Builder);
    addValueBaseName(Builder, EED->getBaseIdentifier());

    // Enum element is of function type; (Self.type) -> Self or
    // (Self.Type) -> (Args...) -> Self.
    Type EnumType = getTypeOfMember(EED, dynamicLookupInfo);
    if (EnumType->is<AnyFunctionType>())
      EnumType = EnumType->castTo<AnyFunctionType>()->getResult();

    if (EnumType->is<FunctionType>()) {
      Builder.addLeftParen();
      addCallArgumentPatterns(Builder, EnumType->castTo<FunctionType>(),
                              EED->getParameterList(),
                              EED->getGenericSignatureOfContext());
      Builder.addRightParen();

      // Extract result as the enum type.
      EnumType = EnumType->castTo<FunctionType>()->getResult();
    }

    addTypeAnnotation(Builder, EnumType, EED->getGenericSignatureOfContext());

    if (isUnresolvedMemberIdealType(EnumType))
      Builder.addFlair(CodeCompletionFlairBit::ExpressionSpecific);
  }

  void addKeyword(StringRef Name, Type TypeAnnotation = Type(),
                  SemanticContextKind SK = SemanticContextKind::None,
                  CodeCompletionKeywordKind KeyKind
                    = CodeCompletionKeywordKind::None,
                  unsigned NumBytesToErase = 0) {
    CodeCompletionResultBuilder Builder(
        Sink, CodeCompletionResult::ResultKind::Keyword, SK,
        expectedTypeContext);
    addLeadingDot(Builder);
    Builder.addKeyword(Name);
    Builder.setKeywordKind(KeyKind);
    if (TypeAnnotation)
      addTypeAnnotation(Builder, TypeAnnotation);
    if (NumBytesToErase > 0)
      Builder.setNumBytesToErase(NumBytesToErase);
  }

  void addKeyword(StringRef Name, StringRef TypeAnnotation,
                  CodeCompletionKeywordKind KeyKind
                    = CodeCompletionKeywordKind::None,
                  CodeCompletionFlair flair = {}) {
    CodeCompletionResultBuilder Builder(
        Sink,
        CodeCompletionResult::ResultKind::Keyword,
        SemanticContextKind::None, expectedTypeContext);
    Builder.addFlair(flair);
    addLeadingDot(Builder);
    Builder.addKeyword(Name);
    Builder.setKeywordKind(KeyKind);
    if (!TypeAnnotation.empty())
      Builder.addTypeAnnotation(TypeAnnotation);
  }

  void addDeclAttrParamKeyword(StringRef Name, StringRef Annotation,
                             bool NeedSpecify) {
    CodeCompletionResultBuilder Builder(
        Sink,
        CodeCompletionResult::ResultKind::Keyword,
        SemanticContextKind::None, expectedTypeContext);
    Builder.addDeclAttrParamKeyword(Name, Annotation, NeedSpecify);
  }

  void addDeclAttrKeyword(StringRef Name, StringRef Annotation) {
    CodeCompletionResultBuilder Builder(
        Sink,
        CodeCompletionResult::ResultKind::Keyword,
        SemanticContextKind::None, expectedTypeContext);
    Builder.addDeclAttrKeyword(Name, Annotation);
  }

  /// Add the compound function name for the given function.
  /// Returns \c true if the compound function name is actually used.
  bool addCompoundFunctionNameIfDesiable(AbstractFunctionDecl *AFD,
                                         DeclVisibilityKind Reason,
                                         DynamicLookupInfo dynamicLookupInfo) {
    auto funcTy =
        getTypeOfMember(AFD, dynamicLookupInfo)->getAs<AnyFunctionType>();
    bool dropCurryLevel = funcTy && AFD->getDeclContext()->isTypeContext() &&
        !isImplicitlyCurriedInstanceMethod(AFD);
    if (dropCurryLevel)
      funcTy = funcTy->getResult()->getAs<AnyFunctionType>();

    bool useFunctionReference = PreferFunctionReferencesToCalls;
    if (!useFunctionReference && funcTy) {
      auto maxRel = calculateMaxTypeRelation(funcTy, expectedTypeContext,
                                             CurrDeclContext);
      useFunctionReference =
          maxRel >= CodeCompletionResult::ExpectedTypeRelation::Convertible;
    }
    if (!useFunctionReference)
      return false;

    // Check for duplicates with the adjusted type too.
    if (dropCurryLevel && isDuplicate(AFD, funcTy))
      return true;

    CommandWordsPairs Pairs;
    CodeCompletionResultBuilder Builder(
        Sink, CodeCompletionResult::ResultKind::Declaration,
        getSemanticContext(AFD, Reason, dynamicLookupInfo),
        expectedTypeContext);
    setClangDeclKeywords(AFD, Pairs, Builder);
    Builder.setAssociatedDecl(AFD);

    // Base name
    addLeadingDot(Builder);
    addValueBaseName(Builder, AFD->getBaseName());

    // Add the argument labels.
    const auto ArgLabels = AFD->getName().getArgumentNames();
    if (!ArgLabels.empty()) {
      if (!HaveLParen)
        Builder.addLeftParen();
      else
        Builder.addAnnotatedLeftParen();

      for (auto ArgLabel : ArgLabels) {
        if (ArgLabel.empty())
          Builder.addTextChunk("_");
        else
          Builder.addTextChunk(ArgLabel.str());
        Builder.addTextChunk(":");
      }

      Builder.addRightParen();
    }

    if (funcTy)
      addTypeAnnotation(Builder, funcTy, AFD->getGenericSignatureOfContext());

    return true;
  }

private:

  /// Returns true if duplicate checking is enabled (via
  /// \c shouldCheckForDuplicates) and this decl + type combination has been
  /// checked previously. Returns false otherwise.
  bool isDuplicate(const ValueDecl *D, Type Ty) {
    if (!CheckForDuplicates)
      return false;
    return !PreviouslySeen.insert({D, Ty}).second;
  }

  /// Returns true if duplicate checking is enabled (via
  /// \c shouldCheckForDuplicates) and this decl has been checked previously
  /// with the type according to \c getTypeOfMember. Returns false otherwise.
  bool isDuplicate(const ValueDecl *D, DynamicLookupInfo dynamicLookupInfo) {
    if (!CheckForDuplicates)
      return false;
    Type Ty = getTypeOfMember(D, dynamicLookupInfo);
    return !PreviouslySeen.insert({D, Ty}).second;
  }

public:

  // Implement swift::VisibleDeclConsumer.
  void foundDecl(ValueDecl *D, DeclVisibilityKind Reason,
                 DynamicLookupInfo dynamicLookupInfo) override {
    assert(Reason !=
             DeclVisibilityKind::MemberOfProtocolDerivedByCurrentNominal &&
           "Including derived requirement in non-override lookup");

    if (D->shouldHideFromEditor())
      return;

    if (IsKeyPathExpr && !KeyPathFilter(D, Reason))
      return;

    if (IsSwiftKeyPathExpr && !SwiftKeyPathFilter(D, Reason))
      return;

    // If we've seen this decl+type before (possible when multiple lookups are
    // performed e.g. because of ambiguous base types), bail.
    if (isDuplicate(D, dynamicLookupInfo))
      return;
    
    // FIXME(InterfaceTypeRequest): Remove this.
    (void)D->getInterfaceType();
    switch (Kind) {
    case LookupKind::ValueExpr:
      if (auto *CD = dyn_cast<ConstructorDecl>(D)) {
        // Do we want compound function names here?
        if (addCompoundFunctionNameIfDesiable(CD, Reason, dynamicLookupInfo))
          return;

        if (auto MT = ExprType->getAs<AnyMetatypeType>()) {
          Type Ty = MT->getInstanceType();
          assert(Ty && "Cannot find instance type.");

          // If instance type is type alias, show users that the constructed
          // type is the typealias instead of the underlying type of the alias.
          Optional<Type> Result = None;
          if (!CD->getInterfaceType()->is<ErrorType>() &&
              isa<TypeAliasType>(Ty.getPointer()) &&
              Ty->getDesugaredType() ==
                CD->getResultInterfaceType().getPointer()) {
            Result = Ty;
          }
          // If the expression type is not a static metatype or an archetype, the base
          // is not a type. Direct call syntax is illegal on values, so we only add
          // initializer completions if we do not have a left parenthesis and either
          // the initializer is required, the base type's instance type is not a class,
          // or this is a 'self' or 'super' reference.
          if (IsStaticMetatype || IsUnresolvedMember || Ty->is<ArchetypeType>())
            addConstructorCall(CD, Reason, dynamicLookupInfo, None, Result,
                               /*isOnType*/ true);
          else if ((IsSelfRefExpr || IsSuperRefExpr || !Ty->is<ClassType>() ||
                    CD->isRequired()) && !HaveLParen)
            addConstructorCall(CD, Reason, dynamicLookupInfo, None, Result,
                               /*isOnType*/ false);
          return;
        }
        if (!HaveLParen) {
          auto CDC = dyn_cast<ConstructorDecl>(CurrDeclContext);
          if (!CDC)
            return;

          // For classes, we do not want 'init' completions for 'self' in
          // non-convenience initializers and for 'super' in convenience initializers.
          if (ExprType->is<ClassType>()) {
            if ((IsSelfRefExpr && !CDC->isConvenienceInit()) ||
                (IsSuperRefExpr && CDC->isConvenienceInit()))
              return;
          }
          if (IsSelfRefExpr || IsSuperRefExpr)
            addConstructorCall(CD, Reason, dynamicLookupInfo, None, None,
                               /*IsOnType=*/false);
        }
        return;
      }

      if (HaveLParen)
        return;

      LLVM_FALLTHROUGH;

    case LookupKind::ValueInDeclContext:
    case LookupKind::ImportFromModule:
      if (auto *VD = dyn_cast<VarDecl>(D)) {
        addVarDeclRef(VD, Reason, dynamicLookupInfo);
        return;
      }

      if (auto *FD = dyn_cast<FuncDecl>(D)) {
        // We cannot call operators with a postfix parenthesis syntax.
        if (FD->isBinaryOperator() || FD->isUnaryOperator())
          return;

        // We cannot call accessors.  We use VarDecls and SubscriptDecls to
        // produce completions that refer to getters and setters.
        if (isa<AccessorDecl>(FD))
          return;

        // Do we want compound function names here?
        if (addCompoundFunctionNameIfDesiable(FD, Reason, dynamicLookupInfo))
          return;

        addMethodCall(FD, Reason, dynamicLookupInfo);

        // SE-0253: Callable values of user-defined nominal types.
        if (FD->isCallAsFunctionMethod() && !HaveDot &&
            (!ExprType || !ExprType->is<AnyMetatypeType>())) {
          Type funcType = getTypeOfMember(FD, dynamicLookupInfo)
                              ->castTo<AnyFunctionType>()
                              ->getResult();

          // Check for duplicates with the adjusted type too.
          if (isDuplicate(FD, funcType))
            return;

          addFunctionCallPattern(
              funcType->castTo<AnyFunctionType>(), FD,
              getSemanticContext(FD, Reason, dynamicLookupInfo));
        }
        return;
      }

      if (auto *NTD = dyn_cast<NominalTypeDecl>(D)) {
        addNominalTypeRef(NTD, Reason, dynamicLookupInfo);
        addConstructorCallsForType(NTD->getDeclaredInterfaceType(),
                                   NTD->getName(), Reason, dynamicLookupInfo);
        return;
      }

      if (auto *TAD = dyn_cast<TypeAliasDecl>(D)) {
        addTypeAliasRef(TAD, Reason, dynamicLookupInfo);
        auto type = TAD->mapTypeIntoContext(TAD->getDeclaredInterfaceType());
        if (type->mayHaveMembers())
          addConstructorCallsForType(type, TAD->getName(), Reason,
                                     dynamicLookupInfo);
        return;
      }

      if (auto *GP = dyn_cast<GenericTypeParamDecl>(D)) {
        addGenericTypeParamRef(GP, Reason, dynamicLookupInfo);
        for (auto *protocol : GP->getConformingProtocols())
          addConstructorCallsForType(protocol->getDeclaredInterfaceType(),
                                     GP->getName(), Reason, dynamicLookupInfo);
        return;
      }

      if (auto *AT = dyn_cast<AssociatedTypeDecl>(D)) {
        addAssociatedTypeRef(AT, Reason, dynamicLookupInfo);
        return;
      }

      if (auto *EED = dyn_cast<EnumElementDecl>(D)) {
        addEnumElementRef(EED, Reason, dynamicLookupInfo,
                          /*HasTypeContext=*/false);
        return;
      }

      // Swift key path allows .[0]
      if (auto *SD = dyn_cast<SubscriptDecl>(D)) {
        addSubscriptCall(SD, Reason, dynamicLookupInfo);
        return;
      }
      return;

    case LookupKind::EnumElement:
      handleEnumElement(D, Reason, dynamicLookupInfo);
      return;

    case LookupKind::Type:
    case LookupKind::TypeInDeclContext:
      if (auto *NTD = dyn_cast<NominalTypeDecl>(D)) {
        addNominalTypeRef(NTD, Reason, dynamicLookupInfo);
        return;
      }

      if (auto *GP = dyn_cast<GenericTypeParamDecl>(D)) {
        addGenericTypeParamRef(GP, Reason, dynamicLookupInfo);
        return;
      }

      LLVM_FALLTHROUGH;
    case LookupKind::GenericRequirement:

      if (TypeAliasDecl *TAD = dyn_cast<TypeAliasDecl>(D)) {
        if (Kind == LookupKind::GenericRequirement &&
            !canBeUsedAsRequirementFirstType(BaseType, TAD))
          return;
        addTypeAliasRef(TAD, Reason, dynamicLookupInfo);
        return;
      }

      if (auto *AT = dyn_cast<AssociatedTypeDecl>(D)) {
        addAssociatedTypeRef(AT, Reason, dynamicLookupInfo);
        return;
      }

      return;
    }
  }

  bool handleEnumElement(ValueDecl *D, DeclVisibilityKind Reason,
                         DynamicLookupInfo dynamicLookupInfo) {
    if (auto *EED = dyn_cast<EnumElementDecl>(D)) {
      addEnumElementRef(EED, Reason, dynamicLookupInfo,
                        /*HasTypeContext=*/true);
      return true;
    } else if (auto *ED = dyn_cast<EnumDecl>(D)) {
      llvm::DenseSet<EnumElementDecl *> Elements;
      ED->getAllElements(Elements);
      for (auto *Ele : Elements) {
        addEnumElementRef(Ele, Reason, dynamicLookupInfo,
                          /*HasTypeContext=*/true);
      }
      return true;
    }
    return false;
  }

  bool tryTupleExprCompletions(Type ExprType) {
    auto *TT = ExprType->getAs<TupleType>();
    if (!TT)
      return false;

    unsigned Index = 0;
    for (auto TupleElt : TT->getElements()) {
      CodeCompletionResultBuilder Builder(
          Sink,
          CodeCompletionResult::ResultKind::Pattern,
          SemanticContextKind::CurrentNominal, expectedTypeContext);
      addLeadingDot(Builder);
      if (TupleElt.hasName()) {
        Builder.addBaseName(TupleElt.getName().str());
      } else {
        llvm::SmallString<4> IndexStr;
        {
          llvm::raw_svector_ostream OS(IndexStr);
          OS << Index;
        }
        Builder.addBaseName(IndexStr.str());
      }
      addTypeAnnotation(Builder, TupleElt.getType());
      ++Index;
    }
    return true;
  }

  bool tryFunctionCallCompletions(Type ExprType, const ValueDecl *VD, Optional<SemanticContextKind> SemanticContext = None) {
    ExprType = ExprType->getRValueType();
    if (auto AFT = ExprType->getAs<AnyFunctionType>()) {
      if (auto *AFD = dyn_cast_or_null<AbstractFunctionDecl>(VD)) {
        addFunctionCallPattern(AFT, AFD, SemanticContext);
      } else {
        addFunctionCallPattern(AFT);
      }
      return true;
    }
    return false;
  }

  bool tryModuleCompletions(Type ExprType, bool TypesOnly = false) {
    if (auto MT = ExprType->getAs<ModuleType>()) {
      ModuleDecl *M = MT->getModule();

      // Only lookup this module's symbols from the cache if it is not the
      // current module.
      if (M == CurrModule)
        return false;

      // If the module is shadowed by a separately imported overlay(s), look up
      // the symbols from the overlay(s) instead.
      SmallVector<ModuleDecl *, 1> ShadowingOrOriginal;
      if (auto *SF = CurrDeclContext->getParentSourceFile()) {
        SF->getSeparatelyImportedOverlays(M, ShadowingOrOriginal);
        if (ShadowingOrOriginal.empty())
          ShadowingOrOriginal.push_back(M);
      }
      for (ModuleDecl *M: ShadowingOrOriginal) {
        RequestedResultsTy Request = RequestedResultsTy::fromModule(M)
            .needLeadingDot(needDot())
            .withModuleQualifier(false);
        if (TypesOnly)
          Request = Request.onlyTypes();
        RequestedCachedResults.push_back(Request);
      }
      return true;
    }
    return false;
  }

  /// If the given ExprType is optional, this adds completions for the unwrapped
  /// type.
  ///
  /// \return true if the given type was Optional .
  bool tryUnwrappedCompletions(Type ExprType, bool isIUO) {
    // FIXME: consider types convertible to T?.

    ExprType = ExprType->getRValueType();
    // FIXME: We don't always pass down whether a type is from an
    // unforced IUO.
    if (isIUO) {
      if (Type Unwrapped = ExprType->getOptionalObjectType()) {
        lookupVisibleMemberDecls(*this, Unwrapped, CurrDeclContext,
                                 IncludeInstanceMembers,
                                 /*includeDerivedRequirements*/false,
                                 /*includeProtocolExtensionMembers*/true);
        return true;
      }
      assert(IsUnwrappedOptional && "IUOs should be optional if not bound/forced");
      return false;
    }

    if (Type Unwrapped = ExprType->getOptionalObjectType()) {
      llvm::SaveAndRestore<bool> ChangeNeedOptionalUnwrap(NeedOptionalUnwrap,
                                                          true);
      if (DotLoc.isValid()) {
        // Let's not erase the dot if the completion is after a swift key path
        // root because \A?.?.member is the correct way to access wrapped type
        // member from an optional key path root.
        auto loc = IsAfterSwiftKeyPathRoot ? DotLoc.getAdvancedLoc(1) : DotLoc;
        NumBytesToEraseForOptionalUnwrap = Ctx.SourceMgr.getByteDistance(
            loc, Ctx.SourceMgr.getCodeCompletionLoc());
      } else {
        NumBytesToEraseForOptionalUnwrap = 0;
      }
      if (NumBytesToEraseForOptionalUnwrap <=
          CodeCompletionResult::MaxNumBytesToErase) {
        if (!tryTupleExprCompletions(Unwrapped)) {
          lookupVisibleMemberDecls(*this, Unwrapped, CurrDeclContext,
                                   IncludeInstanceMembers,
                                   /*includeDerivedRequirements*/false,
                                   /*includeProtocolExtensionMembers*/true);
        }
      }
      return true;
    }
    return false;
  }

  void getPostfixKeywordCompletions(Type ExprType, Expr *ParsedExpr) {
    if (IsSuperRefExpr)
      return;

    if (!ExprType->getAs<ModuleType>()) {
      addKeyword(getTokenText(tok::kw_self), ExprType->getRValueType(),
                 SemanticContextKind::CurrentNominal,
                 CodeCompletionKeywordKind::kw_self);
    }

    if (isa<TypeExpr>(ParsedExpr)) {
      if (auto *T = ExprType->getAs<AnyMetatypeType>()) {
        auto instanceTy = T->getInstanceType();
        if (instanceTy->isAnyExistentialType()) {
          addKeyword("Protocol", MetatypeType::get(instanceTy),
                     SemanticContextKind::CurrentNominal);
          addKeyword("Type", ExistentialMetatypeType::get(instanceTy),
                     SemanticContextKind::CurrentNominal);
        } else {
          addKeyword("Type", MetatypeType::get(instanceTy),
                     SemanticContextKind::CurrentNominal);
        }
      }
    }
  }

  void getValueExprCompletions(Type ExprType, ValueDecl *VD = nullptr) {
    Kind = LookupKind::ValueExpr;
    NeedLeadingDot = !HaveDot;

    ExprType = ExprType->getRValueType();
    assert(!ExprType->hasTypeParameter());

    this->ExprType = ExprType;

    // Open existential types, so that lookupVisibleMemberDecls() can properly
    // substitute them.
    bool WasOptional = false;
    if (auto OptionalType = ExprType->getOptionalObjectType()) {
      ExprType = OptionalType;
      WasOptional = true;
    }

    if (!ExprType->getMetatypeInstanceType()->isAnyObject())
      if (ExprType->isAnyExistentialType())
        ExprType = OpenedArchetypeType::getAny(ExprType);

    if (!IsSelfRefExpr && !IsSuperRefExpr && ExprType->getAnyNominal() &&
        ExprType->getAnyNominal()->isActor()) {
      IsCrossActorReference = true;
    }

    if (WasOptional)
      ExprType = OptionalType::get(ExprType);

    // Handle special cases
    bool isIUO = VD && VD->isImplicitlyUnwrappedOptional();
    if (tryFunctionCallCompletions(ExprType, VD))
      return;
    if (tryModuleCompletions(ExprType))
      return;
    if (tryTupleExprCompletions(ExprType))
      return;
    // Don't check/return so we still add the members of Optional itself below
    tryUnwrappedCompletions(ExprType, isIUO);

    lookupVisibleMemberDecls(*this, ExprType, CurrDeclContext,
                             IncludeInstanceMembers,
                             /*includeDerivedRequirements*/false,
                             /*includeProtocolExtensionMembers*/true);
  }

  void collectOperators(SmallVectorImpl<OperatorDecl *> &results) {
    assert(CurrDeclContext);
    for (auto import : namelookup::getAllImports(CurrDeclContext))
      import.importedModule->getOperatorDecls(results);
  }

  void addPostfixBang(Type resultType) {
    CodeCompletionResultBuilder builder(
        Sink, CodeCompletionResult::ResultKind::BuiltinOperator,
        SemanticContextKind::None, {});
    // FIXME: we can't use the exclamation mark chunk kind, or it isn't
    // included in the completion name.
    builder.addTextChunk("!");
    assert(resultType);
    addTypeAnnotation(builder, resultType);
  }

  void addPostfixOperatorCompletion(OperatorDecl *op, Type resultType) {
    // FIXME: we should get the semantic context of the function, not the
    // operator decl.
    auto semanticContext =
        getSemanticContext(op, DeclVisibilityKind::VisibleAtTopLevel, {});
    CodeCompletionResultBuilder builder(
        Sink, CodeCompletionResult::ResultKind::Declaration, semanticContext,
        {});

    // FIXME: handle variable amounts of space.
    if (HaveLeadingSpace)
      builder.setNumBytesToErase(1);
    builder.setAssociatedDecl(op);
    builder.addBaseName(op->getName().str());
    assert(resultType);
    addTypeAnnotation(builder, resultType);
  }

  void tryPostfixOperator(Expr *expr, PostfixOperatorDecl *op) {
    ConcreteDeclRef referencedDecl;
    FunctionType *funcTy = getTypeOfCompletionOperator(
        const_cast<DeclContext *>(CurrDeclContext), expr, op->getName(),
        DeclRefKind::PostfixOperator, referencedDecl);
    if (!funcTy)
      return;

    // TODO: Use referencedDecl (FuncDecl) instead of 'op' (OperatorDecl).
    addPostfixOperatorCompletion(op, funcTy->getResult());
  }

  void addAssignmentOperator(Type RHSType, Type resultType) {
    CodeCompletionResultBuilder builder(
        Sink, CodeCompletionResult::ResultKind::BuiltinOperator,
        SemanticContextKind::None, {});

    if (HaveLeadingSpace)
      builder.addAnnotatedWhitespace(" ");
    else
      builder.addWhitespace(" ");
    builder.addEqual();
    builder.addWhitespace(" ");
    assert(RHSType && resultType);
    Type contextTy;
    if (auto typeContext = CurrDeclContext->getInnermostTypeContext())
      contextTy = typeContext->getDeclaredTypeInContext();
    builder.addCallParameter(Identifier(), RHSType, contextTy);
    addTypeAnnotation(builder, resultType);
  }

  void addInfixOperatorCompletion(OperatorDecl *op, Type resultType,
                                  Type RHSType) {
    // FIXME: we should get the semantic context of the function, not the
    // operator decl.
    auto semanticContext =
        getSemanticContext(op, DeclVisibilityKind::VisibleAtTopLevel, {});
    CodeCompletionResultBuilder builder(
        Sink, CodeCompletionResult::ResultKind::Declaration, semanticContext,
        {});
    builder.setAssociatedDecl(op);

    if (HaveLeadingSpace)
      builder.addAnnotatedWhitespace(" ");
    else
      builder.addWhitespace(" ");
    builder.addBaseName(op->getName().str());
    builder.addWhitespace(" ");
    if (RHSType) {
      Type contextTy;
      if (auto typeContext = CurrDeclContext->getInnermostTypeContext())
        contextTy = typeContext->getDeclaredTypeInContext();
      builder.addCallParameter(Identifier(), RHSType, contextTy);
    }
    if (resultType)
      addTypeAnnotation(builder, resultType);
  }

  void tryInfixOperatorCompletion(Expr *foldedExpr, InfixOperatorDecl *op) {
    ConcreteDeclRef referencedDecl;
    FunctionType *funcTy = getTypeOfCompletionOperator(
        const_cast<DeclContext *>(CurrDeclContext), foldedExpr, op->getName(),
        DeclRefKind::BinaryOperator, referencedDecl);
    if (!funcTy)
      return;

    Type lhsTy = funcTy->getParams()[0].getPlainType();
    Type rhsTy = funcTy->getParams()[1].getPlainType();
    Type resultTy = funcTy->getResult();

    // Don't complete optional operators on non-optional types.
    if (!lhsTy->getRValueType()->getOptionalObjectType()) {
      // 'T ?? T'
      if (op->getName().str() == "??")
        return;
      // 'T == nil'
      if (auto NT = rhsTy->getNominalOrBoundGenericNominal())
        if (NT->getName() ==
            CurrDeclContext->getASTContext().Id_OptionalNilComparisonType)
          return;
    }

    // If the right-hand side and result type are both type parameters, we're
    // not providing a useful completion.
    if (resultTy->isTypeParameter() && rhsTy->isTypeParameter())
      return;

    // TODO: Use referencedDecl (FuncDecl) instead of 'op' (OperatorDecl).
    addInfixOperatorCompletion(op, funcTy->getResult(),
                               funcTy->getParams()[1].getPlainType());
  }

  Expr *typeCheckLeadingSequence(Expr *LHS, ArrayRef<Expr *> leadingSequence) {
    if (leadingSequence.empty())
      return LHS;

    SourceRange sequenceRange(leadingSequence.front()->getStartLoc(),
                              LHS->getEndLoc());
    auto *expr = findParsedExpr(CurrDeclContext, sequenceRange);
    if (!expr)
      return LHS;

    if (expr->getType() && !expr->getType()->hasError())
      return expr;

    if (!typeCheckExpression(const_cast<DeclContext *>(CurrDeclContext), expr))
      return expr;
    return LHS;
  }

  void getOperatorCompletions(Expr *LHS, ArrayRef<Expr *> leadingSequence) {
    if (IsSuperRefExpr)
      return;

    Expr *foldedExpr = typeCheckLeadingSequence(LHS, leadingSequence);

    SmallVector<OperatorDecl *, 16> operators;
    collectOperators(operators);
    // FIXME: this always chooses the first operator with the given name.
    llvm::DenseSet<Identifier> seenPostfixOperators;
    llvm::DenseSet<Identifier> seenInfixOperators;

    for (auto op : operators) {
      switch (op->getKind()) {
      case DeclKind::PrefixOperator:
        // Don't insert prefix operators in postfix position.
        // FIXME: where should these get completed?
        break;
      case DeclKind::PostfixOperator:
        if (seenPostfixOperators.insert(op->getName()).second)
          tryPostfixOperator(LHS, cast<PostfixOperatorDecl>(op));
        break;
      case DeclKind::InfixOperator:
        if (seenInfixOperators.insert(op->getName()).second)
          tryInfixOperatorCompletion(foldedExpr, cast<InfixOperatorDecl>(op));
        break;
      default:
        llvm_unreachable("unexpected operator kind");
      }
    }

    if (leadingSequence.empty() && LHS->getType() &&
        LHS->getType()->hasLValueType()) {
      addAssignmentOperator(LHS->getType()->getRValueType(),
                            CurrDeclContext->getASTContext().TheEmptyTupleType);
    }

    // FIXME: unify this with the ?.member completions.
    if (auto T = LHS->getType())
      if (auto ValueT = T->getRValueType()->getOptionalObjectType())
        addPostfixBang(ValueT);
  }

  void addTypeRelationFromProtocol(CodeCompletionResultBuilder &builder,
                                   CodeCompletionLiteralKind kind) {
    // Check for matching ExpectedTypes.
    auto *P = Ctx.getProtocol(protocolForLiteralKind(kind));
    for (auto T : expectedTypeContext.possibleTypes) {
      if (!T)
        continue;

      auto typeRelation = CodeCompletionResult::Identical;
      // Convert through optional types unless we're looking for a protocol
      // that Optional itself conforms to.
      if (kind != CodeCompletionLiteralKind::NilLiteral) {
        if (auto optionalObjT = T->getOptionalObjectType()) {
          T = optionalObjT;
          typeRelation = CodeCompletionResult::Convertible;
        }
      }

      // Check for conformance to the literal protocol.
      if (auto *NTD = T->getAnyNominal()) {
        SmallVector<ProtocolConformance *, 2> conformances;
        if (NTD->lookupConformance(CurrModule, P, conformances)) {
          addTypeAnnotation(builder, T);
          builder.setExpectedTypeRelation(typeRelation);
          return;
        }
      }
    }

    // Fallback to showing the default type.
    if (auto defaultTy = defaultTypeLiteralKind(kind, Ctx)) {
      builder.addTypeAnnotation(defaultTy, PrintOptions());
      builder.setExpectedTypeRelation(
          expectedTypeContext.possibleTypes.empty()
              ? CodeCompletionResult::ExpectedTypeRelation::Unknown
              : CodeCompletionResult::ExpectedTypeRelation::Unrelated);
    }
  }

  /// Add '#file', '#line', et at.
  void addPoundLiteralCompletions(bool needPound) {
    CodeCompletionFlair flair;
    if (isCodeCompletionAtTopLevelOfLibraryFile(CurrDeclContext))
      flair |= CodeCompletionFlairBit::ExpressionAtNonScriptOrMainFileScope;

    auto addFromProto = [&](MagicIdentifierLiteralExpr::Kind magicKind,
                            Optional<CodeCompletionLiteralKind> literalKind) {
      CodeCompletionKeywordKind kwKind;
      switch (magicKind) {
      case MagicIdentifierLiteralExpr::FileIDSpelledAsFile:
        kwKind = CodeCompletionKeywordKind::pound_file;
        break;
      case MagicIdentifierLiteralExpr::FilePathSpelledAsFile:
        // Already handled by above case.
        return;
#define MAGIC_IDENTIFIER_TOKEN(NAME, TOKEN) \
      case MagicIdentifierLiteralExpr::NAME: \
        kwKind = CodeCompletionKeywordKind::TOKEN; \
        break;
#define MAGIC_IDENTIFIER_DEPRECATED_TOKEN(NAME, TOKEN)
#include "swift/AST/MagicIdentifierKinds.def"
      }

      StringRef name = MagicIdentifierLiteralExpr::getKindString(magicKind);
      if (!needPound)
        name = name.substr(1);

      if (!literalKind) {
        // Pointer type
        addKeyword(name, "UnsafeRawPointer", kwKind, flair);
        return;
      }

      CodeCompletionResultBuilder builder(
          Sink, CodeCompletionResult::ResultKind::Keyword,
          SemanticContextKind::None, {});
      builder.addFlair(flair);
      builder.setLiteralKind(literalKind.getValue());
      builder.setKeywordKind(kwKind);
      builder.addBaseName(name);
      addTypeRelationFromProtocol(builder, literalKind.getValue());
    };

#define MAGIC_STRING_IDENTIFIER(NAME, STRING, SYNTAX_KIND) \
    addFromProto(MagicIdentifierLiteralExpr::NAME, \
                 CodeCompletionLiteralKind::StringLiteral);
#define MAGIC_INT_IDENTIFIER(NAME, STRING, SYNTAX_KIND) \
    addFromProto(MagicIdentifierLiteralExpr::NAME, \
                 CodeCompletionLiteralKind::IntegerLiteral);
#define MAGIC_POINTER_IDENTIFIER(NAME, STRING, SYNTAX_KIND) \
    addFromProto(MagicIdentifierLiteralExpr::NAME, \
                 None);
#include "swift/AST/MagicIdentifierKinds.def"
  }

  void addValueLiteralCompletions() {
    auto &context = CurrDeclContext->getASTContext();

    CodeCompletionFlair flair;
    if (isCodeCompletionAtTopLevelOfLibraryFile(CurrDeclContext))
      flair |= CodeCompletionFlairBit::ExpressionAtNonScriptOrMainFileScope;

    auto addFromProto = [&](
        CodeCompletionLiteralKind kind,
        llvm::function_ref<void(CodeCompletionResultBuilder &)> consumer,
        bool isKeyword = false) {

      CodeCompletionResultBuilder builder(Sink, CodeCompletionResult::Literal,
                                          SemanticContextKind::None, {});
      builder.setLiteralKind(kind);
      builder.addFlair(flair);

      consumer(builder);
      addTypeRelationFromProtocol(builder, kind);
    };

    // FIXME: the pedantically correct way is to resolve Swift.*LiteralType.

    using LK = CodeCompletionLiteralKind;
    using Builder = CodeCompletionResultBuilder;

    // Add literal completions that conform to specific protocols.
    addFromProto(LK::IntegerLiteral, [](Builder &builder) {
      builder.addTextChunk("0");
    });
    addFromProto(LK::BooleanLiteral, [](Builder &builder) {
      builder.addBaseName("true");
    }, /*isKeyword=*/true);
    addFromProto(LK::BooleanLiteral, [](Builder &builder) {
      builder.addBaseName("false");
    }, /*isKeyword=*/true);
    addFromProto(LK::NilLiteral, [](Builder &builder) {
      builder.addBaseName("nil");
    }, /*isKeyword=*/true);
    addFromProto(LK::StringLiteral, [&](Builder &builder) {
      builder.addTextChunk("\"");
      builder.addSimpleNamedParameter("abc");
      builder.addTextChunk("\"");
    });
    addFromProto(LK::ArrayLiteral, [&](Builder &builder) {
      builder.addLeftBracket();
      builder.addSimpleNamedParameter("values");
      builder.addRightBracket();
    });
    addFromProto(LK::DictionaryLiteral, [&](Builder &builder) {
      builder.addLeftBracket();
      builder.addSimpleNamedParameter("key");
      builder.addTextChunk(": ");
      builder.addSimpleNamedParameter("value");
      builder.addRightBracket();
    });

    // Optionally add object literals.
    if (CompletionContext->includeObjectLiterals()) {
      auto floatType = context.getFloatType();
      addFromProto(LK::ColorLiteral, [&](Builder &builder) {
        builder.addBaseName("#colorLiteral");
        builder.addLeftParen();
        builder.addCallParameter(context.getIdentifier("red"), floatType);
        builder.addComma();
        builder.addCallParameter(context.getIdentifier("green"), floatType);
        builder.addComma();
        builder.addCallParameter(context.getIdentifier("blue"), floatType);
        builder.addComma();
        builder.addCallParameter(context.getIdentifier("alpha"), floatType);
        builder.addRightParen();
      });

      auto stringType = context.getStringType();
      addFromProto(LK::ImageLiteral, [&](Builder &builder) {
        builder.addBaseName("#imageLiteral");
        builder.addLeftParen();
        builder.addCallParameter(context.getIdentifier("resourceName"),
                                 stringType);
        builder.addRightParen();
      });
    }

    // Add tuple completion (item, item).
    {
      CodeCompletionResultBuilder builder(Sink, CodeCompletionResult::Literal,
                                          SemanticContextKind::None, {});
      builder.setLiteralKind(LK::Tuple);
      builder.addFlair(flair);

      builder.addLeftParen();
      builder.addSimpleNamedParameter("values");
      builder.addRightParen();
      for (auto T : expectedTypeContext.possibleTypes) {
        if (T && T->is<TupleType>() && !T->isVoid()) {
          addTypeAnnotation(builder, T);
          builder.setExpectedTypeRelation(CodeCompletionResult::Identical);
          break;
        }
      }
    }
  }

  void addObjCPoundKeywordCompletions(bool needPound) {
    if (!Ctx.LangOpts.EnableObjCInterop)
      return;

    // If the expected type is ObjectiveC.Selector, add #selector. If
    // it's String, add #keyPath.
    bool addedSelector = false;
    bool addedKeyPath = false;

    for (auto T : expectedTypeContext.possibleTypes) {
      T = T->lookThroughAllOptionalTypes();
      if (auto structDecl = T->getStructOrBoundGenericStruct()) {
        if (!addedSelector && structDecl->getName() == Ctx.Id_Selector &&
            structDecl->getParentModule()->getName() == Ctx.Id_ObjectiveC) {
          addPoundSelector(needPound);
          if (addedKeyPath)
            break;
          addedSelector = true;
          continue;
        }
      }

      if (!addedKeyPath && T->isString()) {
        addPoundKeyPath(needPound);
        if (addedSelector)
          break;
        addedKeyPath = true;
        continue;
      }
    }
  }

  struct FilteredDeclConsumer : public swift::VisibleDeclConsumer {
    swift::VisibleDeclConsumer &Consumer;
    DeclFilter Filter;
    FilteredDeclConsumer(swift::VisibleDeclConsumer &Consumer,
                         DeclFilter Filter) : Consumer(Consumer), Filter(Filter) {}
    void foundDecl(ValueDecl *VD, DeclVisibilityKind Kind,
                   DynamicLookupInfo dynamicLookupInfo) override {
      if (Filter(VD, Kind))
        Consumer.foundDecl(VD, Kind, dynamicLookupInfo);
    }
  };

  void getValueCompletionsInDeclContext(SourceLoc Loc,
                                        DeclFilter Filter = DefaultFilter,
                                        bool LiteralCompletions = true,
                                        bool ModuleQualifier = true) {
    ExprType = Type();
    Kind = LookupKind::ValueInDeclContext;
    NeedLeadingDot = false;

    AccessFilteringDeclConsumer AccessFilteringConsumer(
        CurrDeclContext, *this);
    FilteredDeclConsumer FilteringConsumer(AccessFilteringConsumer, Filter);

    lookupVisibleDecls(FilteringConsumer, CurrDeclContext,
                       /*IncludeTopLevel=*/false, Loc);
    RequestedCachedResults.push_back(RequestedResultsTy::toplevelResults()
                                         .withModuleQualifier(ModuleQualifier));

    // Manually add any expected nominal types from imported modules so that
    // they get their expected type relation. Don't include protocols, since
    // they can't be initialized from the type name.
    // FIXME: this does not include types that conform to an expected protocol.
    // FIXME: this creates duplicate results.
    for (auto T : expectedTypeContext.possibleTypes) {
      if (auto NT = T->getAs<NominalType>()) {
        if (auto NTD = NT->getDecl()) {
          if (!isa<ProtocolDecl>(NTD) &&
              NTD->getModuleContext() != CurrModule) {
            addNominalTypeRef(NT->getDecl(),
                              DeclVisibilityKind::VisibleAtTopLevel, {});
          }
        }
      }
    }

    if (CompletionContext) {
      // FIXME: this is an awful simplification that says all and only enums can
      // use implicit member syntax (leading dot). Computing the accurate answer
      // using lookup (e.g. getUnresolvedMemberCompletions) is too expensive,
      // and for some clients this approximation is good enough.
      CompletionContext->MayUseImplicitMemberExpr =
          std::any_of(expectedTypeContext.possibleTypes.begin(),
                      expectedTypeContext.possibleTypes.end(),
          [](Type T) {
            if (auto *NTD = T->getAnyNominal())
              return isa<EnumDecl>(NTD);
            return false;
          });
    }

    if (LiteralCompletions) {
      addValueLiteralCompletions();
      addPoundLiteralCompletions(/*needPound=*/true);
    }

    addObjCPoundKeywordCompletions(/*needPound=*/true);
  }

  void getUnresolvedMemberCompletions(Type T) {
    if (!T->mayHaveMembers())
      return;

    if (auto objT = T->getOptionalObjectType()) {
      // Add 'nil' keyword with erasing '.' instruction.
      unsigned bytesToErase = 0;
      auto &SM = CurrDeclContext->getASTContext().SourceMgr;
      if (DotLoc.isValid())
        bytesToErase = SM.getByteDistance(DotLoc, SM.getCodeCompletionLoc());
      addKeyword("nil", T, SemanticContextKind::None,
                 CodeCompletionKeywordKind::kw_nil, bytesToErase);
    }

    // We can only say .foo where foo is a static member of the contextual
    // type and has the same type (or if the member is a function, then the
    // same result type) as the contextual type.
    FilteredDeclConsumer consumer(*this, [=](ValueDecl *VD,
                                             DeclVisibilityKind Reason) {
      if (T->getOptionalObjectType() &&
          VD->getModuleContext()->isStdlibModule()) {
        // In optional context, ignore '.init(<some>)', 'init(nilLiteral:)',
        if (isa<ConstructorDecl>(VD))
          return false;
      }
      return true;
    });

    auto baseType = MetatypeType::get(T);
    llvm::SaveAndRestore<LookupKind> SaveLook(Kind, LookupKind::ValueExpr);
    llvm::SaveAndRestore<Type> SaveType(ExprType, baseType);
    llvm::SaveAndRestore<bool> SaveUnresolved(IsUnresolvedMember, true);
    lookupVisibleMemberDecls(consumer, baseType, CurrDeclContext,
                             /*includeInstanceMembers=*/false,
                             /*includeDerivedRequirements*/false,
                             /*includeProtocolExtensionMembers*/true);
  }

  void getUnresolvedMemberCompletions(ArrayRef<Type> Types) {
    NeedLeadingDot = !HaveDot;

    SmallPtrSet<CanType, 4> seenTypes;
    for (auto T : Types) {
      if (!T || !seenTypes.insert(T->getCanonicalType()).second)
        continue;

      if (auto objT = T->getOptionalObjectType()) {
        // If this is optional type, perform completion for the object type.
        // i.e. 'let _: Enum??? = .enumMember' is legal.
        objT = objT->lookThroughAllOptionalTypes();
        if (seenTypes.insert(objT->getCanonicalType()).second)
          getUnresolvedMemberCompletions(objT);
      }
      getUnresolvedMemberCompletions(T);
    }
  }

  void addCallArgumentCompletionResults(
      ArrayRef<PossibleParamInfo> ParamInfos,
      bool isLabeledTrailingClosure = false) {
    Type ContextType;
    if (auto typeContext = CurrDeclContext->getInnermostTypeContext())
      ContextType = typeContext->getDeclaredTypeInContext();

    for (auto Info : ParamInfos) {
      const auto *Arg = Info.Param;
      if (!Arg)
        continue;
      CodeCompletionResultBuilder Builder(
          Sink, CodeCompletionResult::ResultKind::Pattern,
          // FIXME: SemanticContextKind::Local is not correct.
          // Use 'None' (and fix prioritization) or introduce a new context.
          SemanticContextKind::Local, {});
      Builder.addCallParameter(Arg->getLabel(), Identifier(),
                               Arg->getPlainType(), ContextType,
                               Arg->isVariadic(), Arg->isInOut(),
                               /*isIUO=*/false, Arg->isAutoClosure(),
                               /*useUnderscoreLabel=*/true,
                               isLabeledTrailingClosure);
      Builder.addFlair(CodeCompletionFlairBit::ArgumentLabels);
      auto Ty = Arg->getPlainType();
      if (Arg->isInOut()) {
        Ty = InOutType::get(Ty);
      } else if (Arg->isAutoClosure()) {
        // 'Ty' may be ErrorType.
        if (auto funcTy = Ty->getAs<FunctionType>())
          Ty = funcTy->getResult();
      }
      addTypeAnnotation(Builder, Ty);
      Builder.setExpectedTypeRelation(
          CodeCompletionResult::ExpectedTypeRelation::NotApplicable);
    }
  }

  void getTypeCompletions(Type BaseType) {
    if (tryModuleCompletions(BaseType, /*OnlyTypes=*/true))
      return;
    Kind = LookupKind::Type;
    this->BaseType = BaseType;
    NeedLeadingDot = !HaveDot;
    lookupVisibleMemberDecls(*this, MetatypeType::get(BaseType),
                             CurrDeclContext,
                             IncludeInstanceMembers,
                             /*includeDerivedRequirements*/false,
                             /*includeProtocolExtensionMembers*/false);
    if (BaseType->isAnyExistentialType()) {
      addKeyword("Protocol", MetatypeType::get(BaseType));
      addKeyword("Type", ExistentialMetatypeType::get(BaseType));
    } else if (!BaseType->is<ModuleType>()) {
      addKeyword("Type", MetatypeType::get(BaseType));
    }
  }

  void getGenericRequirementCompletions(DeclContext *DC,
                                        SourceLoc CodeCompletionLoc) {
    auto genericSig = DC->getGenericSignatureOfContext();
    if (!genericSig)
      return;

    for (auto GPT : genericSig->getGenericParams()) {
      addGenericTypeParamRef(GPT->getDecl(),
                             DeclVisibilityKind::GenericParameter, {});
    }

    // For non-protocol nominal type decls, only suggest generic parameters.
    if (auto D = DC->getAsDecl())
      if (isa<NominalTypeDecl>(D) && !isa<ProtocolDecl>(D))
        return;

    auto typeContext = DC->getInnermostTypeContext();
    if (!typeContext)
      return;

    auto selfTy = typeContext->getSelfTypeInContext();
    Kind = LookupKind::GenericRequirement;
    this->BaseType = selfTy;
    NeedLeadingDot = false;
    lookupVisibleMemberDecls(*this, MetatypeType::get(selfTy),
                             CurrDeclContext, IncludeInstanceMembers,
                             /*includeDerivedRequirements*/false,
                             /*includeProtocolExtensionMembers*/true);
    // We not only allow referencing nested types/typealiases directly, but also
    // qualified by the current type. Thus also suggest current self type so the
    // user can do a memberwise lookup on it.
    if (auto SelfType = typeContext->getSelfNominalTypeDecl()) {
      addNominalTypeRef(SelfType, DeclVisibilityKind::LocalVariable,
                        DynamicLookupInfo());
    }

    // Self is also valid in all cases in which it can be used in function
    // bodies. Suggest it if applicable.
    getSelfTypeCompletionInDeclContext(CodeCompletionLoc,
                                       /*isForResultType=*/false);
  }

  static bool canUseAttributeOnDecl(DeclAttrKind DAK, bool IsInSil,
                                    bool IsConcurrencyEnabled,
                                    bool IsDistributedEnabled,
                                    Optional<DeclKind> DK) {
    if (DeclAttribute::isUserInaccessible(DAK))
      return false;
    if (DeclAttribute::isDeclModifier(DAK))
      return false;
    if (DeclAttribute::shouldBeRejectedByParser(DAK))
      return false;
    if (!IsInSil && DeclAttribute::isSilOnly(DAK))
      return false;
    if (!IsConcurrencyEnabled && DeclAttribute::isConcurrencyOnly(DAK))
      return false;
    if (!IsDistributedEnabled && DeclAttribute::isDistributedOnly(DAK))
      return false;
    if (!DK.hasValue())
      return true;
    return DeclAttribute::canAttributeAppearOnDeclKind(DAK, DK.getValue());
  }

  void getAttributeDeclCompletions(bool IsInSil, Optional<DeclKind> DK) {
    // FIXME: also include user-defined attribute keywords
    StringRef TargetName = "Declaration";
    if (DK.hasValue()) {
      switch (DK.getValue()) {
#define DECL(Id, ...)                                                         \
      case DeclKind::Id:                                                      \
        TargetName = #Id;                                                     \
        break;
#include "swift/AST/DeclNodes.def"
      }
    }
    bool IsConcurrencyEnabled = Ctx.LangOpts.EnableExperimentalConcurrency;
    bool IsDistributedEnabled = Ctx.LangOpts.EnableExperimentalDistributed;
    std::string Description = TargetName.str() + " Attribute";
#define DECL_ATTR(KEYWORD, NAME, ...)                                         \
    if (canUseAttributeOnDecl(DAK_##NAME, IsInSil,                            \
                              IsConcurrencyEnabled,                           \
                              IsDistributedEnabled,                           \
                              DK))                                            \
      addDeclAttrKeyword(#KEYWORD, Description);
#include "swift/AST/Attr.def"
  }

  void getAttributeDeclParamCompletions(DeclAttrKind AttrKind, int ParamIndex) {
    if (AttrKind == DAK_Available) {
      if (ParamIndex == 0) {
        addDeclAttrParamKeyword("*", "Platform", false);

#define AVAILABILITY_PLATFORM(X, PrettyName)                                  \
        addDeclAttrParamKeyword(swift::platformString(PlatformKind::X),       \
                                "Platform", false);
#include "swift/AST/PlatformKinds.def"

      } else {
        addDeclAttrParamKeyword("unavailable", "", false);
        addDeclAttrParamKeyword("message", "Specify message", true);
        addDeclAttrParamKeyword("renamed", "Specify replacing name", true);
        addDeclAttrParamKeyword("introduced", "Specify version number", true);
        addDeclAttrParamKeyword("deprecated", "Specify version number", true);
      }
    }
  }

  void getTypeAttributeKeywordCompletions() {
    auto addTypeAttr = [&](StringRef Name) {
      CodeCompletionResultBuilder Builder(
          Sink,
          CodeCompletionResult::ResultKind::Keyword,
          SemanticContextKind::None, expectedTypeContext);
      Builder.addAttributeKeyword(Name, "Type Attribute");
    };
    addTypeAttr("autoclosure");
    addTypeAttr("convention(swift)");
    addTypeAttr("convention(block)");
    addTypeAttr("convention(c)");
    addTypeAttr("convention(thin)");
    addTypeAttr("escaping");
  }

  void collectPrecedenceGroups() {
    assert(CurrDeclContext);

    if (CurrModule) {
      for (auto FU: CurrModule->getFiles()) {
        // We are looking through the current module,
        // inspect only source files.
        if (FU->getKind() != FileUnitKind::Source)
          continue;

        llvm::SmallVector<PrecedenceGroupDecl*, 4> results;
        cast<SourceFile>(FU)->getPrecedenceGroups(results);

        for (auto PG: results)
            addPrecedenceGroupRef(PG);
      }
    }
    for (auto Import : namelookup::getAllImports(CurrDeclContext)) {
      auto Module = Import.importedModule;
      if (Module == CurrModule)
        continue;

      RequestedCachedResults.push_back(RequestedResultsTy::fromModule(Module)
                                           .onlyPrecedenceGroups()
                                           .withModuleQualifier(false));
    }
  }

  void getPrecedenceGroupCompletions(SyntaxKind SK) {
    switch (SK) {
    case SyntaxKind::PrecedenceGroupAssociativity:
      addKeyword(getAssociativitySpelling(Associativity::None));
      addKeyword(getAssociativitySpelling(Associativity::Left));
      addKeyword(getAssociativitySpelling(Associativity::Right));
      break;
    case SyntaxKind::PrecedenceGroupAssignment:
      addKeyword(getTokenText(tok::kw_false), Type(), SemanticContextKind::None,
                 CodeCompletionKeywordKind::kw_false);
      addKeyword(getTokenText(tok::kw_true), Type(), SemanticContextKind::None,
                 CodeCompletionKeywordKind::kw_true);
      break;
    case SyntaxKind::PrecedenceGroupAttributeList:
      addKeyword("associativity");
      addKeyword("higherThan");
      addKeyword("lowerThan");
      addKeyword("assignment");
      break;
    case SyntaxKind::PrecedenceGroupRelation:
      collectPrecedenceGroups();
      break;
    default:
        llvm_unreachable("not a precedencegroup SyntaxKind");
    }
  }

  void getPoundAvailablePlatformCompletions() {

    // The platform names should be identical to those in @available.
    getAttributeDeclParamCompletions(DAK_Available, 0);
  }

  /// \p Loc is the location of the code completin token.
  /// \p isForDeclResult determines if were are spelling out the result type
  /// of a declaration.
  void getSelfTypeCompletionInDeclContext(SourceLoc Loc, bool isForDeclResult) {
    const DeclContext *typeDC = CurrDeclContext->getInnermostTypeContext();
    if (!typeDC)
      return;

    // For protocols, there's a 'Self' generic parameter.
    if (typeDC->getSelfProtocolDecl())
      return;

    Type selfType =
        CurrDeclContext->mapTypeIntoContext(typeDC->getSelfInterfaceType());

    if (typeDC->getSelfClassDecl()) {
      // In classes, 'Self' can be used in result type of func, subscript and
      // computed property, or inside function bodies.
      bool canUseDynamicSelf = false;
      if (isForDeclResult) {
        canUseDynamicSelf = true;
      } else {
        const auto *checkDC = CurrDeclContext;
        if (isa<TypeAliasDecl>(checkDC))
          checkDC = checkDC->getParent();

        if (const auto *AFD = checkDC->getInnermostMethodContext()) {
          canUseDynamicSelf = Ctx.SourceMgr.rangeContainsTokenLoc(
              AFD->getBodySourceRange(), Loc);
        }
      }
      if (!canUseDynamicSelf)
        return;
      // 'Self' in class is a dynamic type.
      selfType = DynamicSelfType::get(selfType, Ctx);
    } else {
      // In enums and structs, 'Self' is just an alias for the nominal type,
      // and can be used anywhere.
    }

    CodeCompletionResultBuilder Builder(
        Sink, CodeCompletionResult::ResultKind::Keyword,
        SemanticContextKind::CurrentNominal, expectedTypeContext);
    Builder.addKeyword("Self");
    Builder.setKeywordKind(CodeCompletionKeywordKind::kw_Self);
    addTypeAnnotation(Builder, selfType);
  }

  void getTypeCompletionsInDeclContext(SourceLoc Loc,
                                       bool ModuleQualifier = true) {
    Kind = LookupKind::TypeInDeclContext;

    AccessFilteringDeclConsumer AccessFilteringConsumer(
        CurrDeclContext, *this);
    lookupVisibleDecls(AccessFilteringConsumer, CurrDeclContext,
                       /*IncludeTopLevel=*/false, Loc);

    RequestedCachedResults.push_back(
      RequestedResultsTy::toplevelResults()
        .onlyTypes()
        .withModuleQualifier(ModuleQualifier));
  }

  void getToplevelCompletions(bool OnlyTypes) {
    Kind = OnlyTypes ? LookupKind::TypeInDeclContext
                     : LookupKind::ValueInDeclContext;
    NeedLeadingDot = false;

    UsableFilteringDeclConsumer UsableFilteringConsumer(
        Ctx.SourceMgr, CurrDeclContext, Ctx.SourceMgr.getCodeCompletionLoc(),
        *this);
    AccessFilteringDeclConsumer AccessFilteringConsumer(
        CurrDeclContext, UsableFilteringConsumer);

    CurrModule->lookupVisibleDecls({}, AccessFilteringConsumer,
                                   NLKind::UnqualifiedLookup);
  }

  void lookupExternalModuleDecls(const ModuleDecl *TheModule,
                                 ArrayRef<std::string> AccessPath,
                                 bool ResultsHaveLeadingDot) {
    assert(CurrModule != TheModule &&
           "requested module should be external");

    Kind = LookupKind::ImportFromModule;
    NeedLeadingDot = ResultsHaveLeadingDot;

    ImportPath::Access::Builder builder;
    for (auto Piece : AccessPath) {
      builder.push_back(Ctx.getIdentifier(Piece));
    }

    AccessFilteringDeclConsumer FilteringConsumer(CurrDeclContext, *this);
    TheModule->lookupVisibleDecls(builder.get(), FilteringConsumer,
                                  NLKind::UnqualifiedLookup);

    llvm::SmallVector<PrecedenceGroupDecl*, 16> precedenceGroups;
    TheModule->getPrecedenceGroups(precedenceGroups);

    for (auto PGD: precedenceGroups)
      addPrecedenceGroupRef(PGD);
  }

  void getStmtLabelCompletions(SourceLoc Loc, bool isContinue) {
    class LabelFinder : public ASTWalker {
      SourceManager &SM;
      SourceLoc TargetLoc;
      bool IsContinue;

    public:
      SmallVector<Identifier, 2> Result;

      LabelFinder(SourceManager &SM, SourceLoc TargetLoc, bool IsContinue)
          : SM(SM), TargetLoc(TargetLoc), IsContinue(IsContinue) {}

      std::pair<bool, Stmt *> walkToStmtPre(Stmt *S) override {
        if (SM.isBeforeInBuffer(S->getEndLoc(), TargetLoc))
          return {false, S};

        if (LabeledStmt *LS = dyn_cast<LabeledStmt>(S)) {
          if (LS->getLabelInfo()) {
            if (!IsContinue || LS->isPossibleContinueTarget()) {
              auto label = LS->getLabelInfo().Name;
              if (!llvm::is_contained(Result, label))
                Result.push_back(label);
            }
          }
        }

        return {true, S};
      }

      Stmt *walkToStmtPost(Stmt *S) override { return nullptr; }

      std::pair<bool, Expr *> walkToExprPre(Expr *E) override {
        if (SM.isBeforeInBuffer(E->getEndLoc(), TargetLoc))
          return {false, E};
        return {true, E};
      }
    } Finder(CurrDeclContext->getASTContext().SourceMgr, Loc, isContinue);
    const_cast<DeclContext *>(CurrDeclContext)->walkContext(Finder);
    for (auto name : Finder.Result) {
      CodeCompletionResultBuilder Builder(
          Sink, CodeCompletionResult::ResultKind::Pattern,
          SemanticContextKind::Local, {});
      Builder.addTextChunk(name.str());
    }
  }
};

class CompletionOverrideLookup : public swift::VisibleDeclConsumer {
  CodeCompletionResultSink &Sink;
  ASTContext &Ctx;
  const DeclContext *CurrDeclContext;
  SmallVectorImpl<StringRef> &ParsedKeywords;
  SourceLoc introducerLoc;

  bool hasFuncIntroducer = false;
  bool hasVarIntroducer = false;
  bool hasTypealiasIntroducer = false;
  bool hasInitializerModifier = false;
  bool hasAccessModifier = false;
  bool hasOverride = false;
  bool hasOverridabilityModifier = false;
  bool hasStaticOrClass = false;

public:
  CompletionOverrideLookup(CodeCompletionResultSink &Sink, ASTContext &Ctx,
                           const DeclContext *CurrDeclContext,
                           SmallVectorImpl<StringRef> &ParsedKeywords,
                           SourceLoc introducerLoc)
      : Sink(Sink), Ctx(Ctx), CurrDeclContext(CurrDeclContext),
        ParsedKeywords(ParsedKeywords), introducerLoc(introducerLoc) {
    hasFuncIntroducer = isKeywordSpecified("func");
    hasVarIntroducer = isKeywordSpecified("var") ||
                       isKeywordSpecified("let");
    hasTypealiasIntroducer = isKeywordSpecified("typealias");
    hasInitializerModifier = isKeywordSpecified("required") ||
                             isKeywordSpecified("convenience");
    hasAccessModifier = isKeywordSpecified("private") ||
                        isKeywordSpecified("fileprivate") ||
                        isKeywordSpecified("internal") ||
                        isKeywordSpecified("public") ||
                        isKeywordSpecified("open");
    hasOverride = isKeywordSpecified("override");
    hasOverridabilityModifier = isKeywordSpecified("final") ||
                                isKeywordSpecified("open");
    hasStaticOrClass = isKeywordSpecified(getTokenText(tok::kw_class)) ||
                       isKeywordSpecified(getTokenText(tok::kw_static));
  }

  bool isKeywordSpecified(StringRef Word) {
    return std::find(ParsedKeywords.begin(), ParsedKeywords.end(), Word)
      != ParsedKeywords.end();
  }

  bool missingOverride(DeclVisibilityKind Reason) {
    return !hasOverride && Reason == DeclVisibilityKind::MemberOfSuper &&
           !CurrDeclContext->getSelfProtocolDecl();
  }

  void addAccessControl(const ValueDecl *VD,
                        CodeCompletionResultBuilder &Builder) {
    auto CurrentNominal = CurrDeclContext->getSelfNominalTypeDecl();
    assert(CurrentNominal);

    auto AccessOfContext = CurrentNominal->getFormalAccess();
    if (AccessOfContext < AccessLevel::Public)
      return;

    auto Access = VD->getFormalAccess();
    // Use the greater access between the protocol requirement and the witness.
    // In case of:
    //
    //   public protocol P { func foo() }
    //   public class B { func foo() {} }
    //   public class C: B, P {
    //     <complete>
    //   }
    //
    // 'VD' is 'B.foo()' which is implicitly 'internal'. But as the overriding
    // declaration, the user needs to write both 'public' and 'override':
    //
    //   public class C: B {
    //     public override func foo() {}
    //   }
    if (Access < AccessLevel::Public &&
        !isa<ProtocolDecl>(VD->getDeclContext())) {
      for (auto Conformance : CurrentNominal->getAllConformances()) {
        Conformance->getRootConformance()->forEachValueWitness(
            [&](ValueDecl *req, Witness witness) {
              if (witness.getDecl() == VD)
                Access = std::max(
                    Access, Conformance->getProtocol()->getFormalAccess());
            });
      }
    }

    Access = std::min(Access, AccessOfContext);
    // Only emit 'public', not needed otherwise.
    if (Access >= AccessLevel::Public)
      Builder.addAccessControlKeyword(Access);
  }

  /// Return type if the result type if \p VD should be represented as opaque
  /// result type.
  Type getOpaqueResultType(const ValueDecl *VD, DeclVisibilityKind Reason,
                           DynamicLookupInfo dynamicLookupInfo) {
    if (Reason !=
        DeclVisibilityKind::MemberOfProtocolConformedToByCurrentNominal)
      return nullptr;

    auto currTy = CurrDeclContext->getDeclaredTypeInContext();
    if (!currTy)
      return nullptr;

    Type ResultT;
    if (auto *FD = dyn_cast<FuncDecl>(VD)) {
      if (FD->getGenericParams()) {
        // Generic function cannot have opaque result type.
        return nullptr;
      }
      ResultT = FD->getResultInterfaceType();
    } else if (auto *SD = dyn_cast<SubscriptDecl>(VD)) {
      if (SD->getGenericParams()) {
        // Generic subscript cannot have opaque result type.
        return nullptr;
      }
      ResultT = SD->getElementInterfaceType();
    } else if (auto *VarD = dyn_cast<VarDecl>(VD)) {
      ResultT = VarD->getInterfaceType();
    } else {
      return nullptr;
    }

    if (!ResultT->is<DependentMemberType>() ||
        !ResultT->castTo<DependentMemberType>()->getAssocType())
      // The result is not a valid associatedtype.
      return nullptr;

    // Try substitution to see if the associated type is resolved to concrete
    // type.
    auto substMap = currTy->getMemberSubstitutionMap(
        CurrDeclContext->getParentModule(), VD);
    if (!ResultT.subst(substMap)->is<DependentMemberType>())
      // If resolved print it.
      return nullptr;

    auto genericSig = VD->getDeclContext()->getGenericSignatureOfContext();

    if (genericSig->isConcreteType(ResultT))
      // If it has same type requrement, we will emit the concrete type.
      return nullptr;

    // Collect requirements on the associatedtype.
    SmallVector<Type, 2> opaqueTypes;
    bool hasExplicitAnyObject = false;
    if (auto superTy = genericSig->getSuperclassBound(ResultT))
      opaqueTypes.push_back(superTy);
    for (const auto proto : genericSig->getRequiredProtocols(ResultT))
      opaqueTypes.push_back(proto->getDeclaredInterfaceType());
    if (auto layout = genericSig->getLayoutConstraint(ResultT))
      hasExplicitAnyObject = layout->isClass();

    if (!hasExplicitAnyObject) {
      if (opaqueTypes.empty())
        return nullptr;
      if (opaqueTypes.size() == 1)
        return opaqueTypes.front();
    }
    return ProtocolCompositionType::get(
        VD->getASTContext(), opaqueTypes, hasExplicitAnyObject);
  }

  void addValueOverride(const ValueDecl *VD, DeclVisibilityKind Reason,
                        DynamicLookupInfo dynamicLookupInfo,
                        CodeCompletionResultBuilder &Builder,
                        bool hasDeclIntroducer) {
    class DeclPrinter : public StreamPrinter {
      Type OpaqueBaseTy;

    public:
      using StreamPrinter::StreamPrinter;

      Optional<unsigned> NameOffset;

      DeclPrinter(raw_ostream &OS, Type OpaqueBaseTy)
          : StreamPrinter(OS), OpaqueBaseTy(OpaqueBaseTy) {}

      void printDeclLoc(const Decl *D) override {
        if (!NameOffset.hasValue())
          NameOffset = OS.tell();
      }

      // As for FuncDecl, SubscriptDecl, and VarDecl,
      void printDeclResultTypePre(ValueDecl *VD, TypeLoc &TL) override {
        if (!OpaqueBaseTy.isNull()) {
          OS << "some ";
          TL = TypeLoc::withoutLoc(OpaqueBaseTy);
        }
      }
    };

    llvm::SmallString<256> DeclStr;
    unsigned NameOffset = 0;
    {
      llvm::raw_svector_ostream OS(DeclStr);
      DeclPrinter Printer(
          OS, getOpaqueResultType(VD, Reason, dynamicLookupInfo));
      PrintOptions Options;
      if (auto transformType = CurrDeclContext->getDeclaredTypeInContext())
        Options.setBaseType(transformType);
      Options.SkipUnderscoredKeywords = true;
      Options.PrintImplicitAttrs = false;
      Options.ExclusiveAttrList.push_back(TAK_escaping);
      Options.ExclusiveAttrList.push_back(TAK_autoclosure);
      Options.PrintOverrideKeyword = false;
      Options.PrintPropertyAccessors = false;
      Options.PrintSubscriptAccessors = false;
      Options.PrintStaticKeyword = !hasStaticOrClass;
      VD->print(Printer, Options);
      NameOffset = Printer.NameOffset.getValue();
    }

    if (!hasDeclIntroducer && !hasAccessModifier)
      addAccessControl(VD, Builder);

    if (missingOverride(Reason)) {
      if (!hasDeclIntroducer)
        Builder.addOverrideKeyword();
      else {
        auto dist = Ctx.SourceMgr.getByteDistance(
            introducerLoc, Ctx.SourceMgr.getCodeCompletionLoc());
        if (dist <= CodeCompletionResult::MaxNumBytesToErase) {
          Builder.setNumBytesToErase(dist);
          Builder.addOverrideKeyword();
          Builder.addDeclIntroducer(DeclStr.str().substr(0, NameOffset));
        }
      }
    }

    if (!hasDeclIntroducer && NameOffset != 0)
      Builder.addDeclIntroducer(DeclStr.str().substr(0, NameOffset));

    Builder.addTextChunk(DeclStr.str().substr(NameOffset));
  }

  void addMethodOverride(const FuncDecl *FD, DeclVisibilityKind Reason,
                         DynamicLookupInfo dynamicLookupInfo) {
    CodeCompletionResultBuilder Builder(
        Sink, CodeCompletionResult::ResultKind::Declaration,
        SemanticContextKind::Super, {});
    Builder.setExpectedTypeRelation(
        CodeCompletionResult::ExpectedTypeRelation::NotApplicable);
    Builder.setAssociatedDecl(FD);
    addValueOverride(FD, Reason, dynamicLookupInfo, Builder, hasFuncIntroducer);
    Builder.addBraceStmtWithCursor();
  }

  void addVarOverride(const VarDecl *VD, DeclVisibilityKind Reason,
                      DynamicLookupInfo dynamicLookupInfo) {
    // Overrides cannot use 'let', but if the 'override' keyword is specified
    // then the intention is clear, so provide the results anyway.  The compiler
    // can then provide an error telling you to use 'var' instead.
    // If we don't need override then it's a protocol requirement, so show it.
    if (missingOverride(Reason) && hasVarIntroducer &&
        isKeywordSpecified("let"))
      return;

    CodeCompletionResultBuilder Builder(
        Sink, CodeCompletionResult::ResultKind::Declaration,
        SemanticContextKind::Super, {});
    Builder.setAssociatedDecl(VD);
    addValueOverride(VD, Reason, dynamicLookupInfo, Builder, hasVarIntroducer);
  }

  void addSubscriptOverride(const SubscriptDecl *SD, DeclVisibilityKind Reason,
                            DynamicLookupInfo dynamicLookupInfo) {
    CodeCompletionResultBuilder Builder(
        Sink, CodeCompletionResult::ResultKind::Declaration,
        SemanticContextKind::Super, {});
    Builder.setExpectedTypeRelation(
        CodeCompletionResult::ExpectedTypeRelation::NotApplicable);
    Builder.setAssociatedDecl(SD);
    addValueOverride(SD, Reason, dynamicLookupInfo, Builder, false);
    Builder.addBraceStmtWithCursor();
  }

  void addTypeAlias(const AssociatedTypeDecl *ATD, DeclVisibilityKind Reason,
                    DynamicLookupInfo dynamicLookupInfo) {
    CodeCompletionResultBuilder Builder(Sink,
      CodeCompletionResult::ResultKind::Declaration,
      SemanticContextKind::Super, {});
    Builder.setExpectedTypeRelation(
        CodeCompletionResult::ExpectedTypeRelation::NotApplicable);
    Builder.setAssociatedDecl(ATD);
    if (!hasTypealiasIntroducer && !hasAccessModifier)
      addAccessControl(ATD, Builder);
    if (!hasTypealiasIntroducer)
      Builder.addDeclIntroducer("typealias ");
    Builder.addTextChunk(ATD->getName().str());
    Builder.addTextChunk(" = ");
    Builder.addSimpleNamedParameter("Type");
  }

  void addConstructor(const ConstructorDecl *CD, DeclVisibilityKind Reason,
                      DynamicLookupInfo dynamicLookupInfo) {
    CodeCompletionResultBuilder Builder(
        Sink,
        CodeCompletionResult::ResultKind::Declaration,
        SemanticContextKind::Super, {});
    Builder.setExpectedTypeRelation(
        CodeCompletionResult::ExpectedTypeRelation::NotApplicable);
    Builder.setAssociatedDecl(CD);

    if (!hasAccessModifier)
      addAccessControl(CD, Builder);

    if (missingOverride(Reason) && CD->isDesignatedInit() && !CD->isRequired())
      Builder.addOverrideKeyword();

    // Emit 'required' if we're in class context, 'required' is not specified,
    // and 1) this is a protocol conformance and the class is not final, or 2)
    // this is subclass and the initializer is marked as required.
    bool needRequired = false;
    auto C = CurrDeclContext->getSelfClassDecl();
    if (C && !isKeywordSpecified("required")) {
      switch (Reason) {
      case DeclVisibilityKind::MemberOfProtocolConformedToByCurrentNominal:
      case DeclVisibilityKind::MemberOfProtocolDerivedByCurrentNominal:
        if (!C->isSemanticallyFinal())
          needRequired = true;
        break;
      case DeclVisibilityKind::MemberOfSuper:
        if (CD->isRequired())
          needRequired = true;
        break;
      default: break;
      }
    }

    llvm::SmallString<256> DeclStr;
    if (needRequired)
      DeclStr += "required ";
    {
      llvm::raw_svector_ostream OS(DeclStr);
      PrintOptions Options;
      if (auto transformType = CurrDeclContext->getDeclaredTypeInContext())
        Options.setBaseType(transformType);
      Options.PrintImplicitAttrs = false;
      Options.SkipAttributes = true;
      CD->print(OS, Options);
    }
    Builder.addTextChunk(DeclStr);
    Builder.addBraceStmtWithCursor();
  }

  // Implement swift::VisibleDeclConsumer.
  void foundDecl(ValueDecl *D, DeclVisibilityKind Reason,
                 DynamicLookupInfo dynamicLookupInfo) override {
    if (Reason == DeclVisibilityKind::MemberOfCurrentNominal)
      return;

    if (D->shouldHideFromEditor())
      return;

    if (D->isSemanticallyFinal())
      return;

    bool hasIntroducer = hasFuncIntroducer ||
                         hasVarIntroducer ||
                         hasTypealiasIntroducer;

    if (hasStaticOrClass && !D->isStatic())
      return;

    // As per the current convention, only instance members are
    // suggested if an introducer is not accompanied by a 'static' or
    // 'class' modifier.
    if (hasIntroducer && !hasStaticOrClass && D->isStatic())
      return;

    if (auto *FD = dyn_cast<FuncDecl>(D)) {
      // We cannot override operators as members.
      if (FD->isBinaryOperator() || FD->isUnaryOperator())
        return;

      // We cannot override individual accessors.
      if (isa<AccessorDecl>(FD))
        return;

      if (hasFuncIntroducer || (!hasIntroducer && !hasInitializerModifier))
        addMethodOverride(FD, Reason, dynamicLookupInfo);
      return;
    }

    if (auto *VD = dyn_cast<VarDecl>(D)) {
      if (hasVarIntroducer || (!hasIntroducer && !hasInitializerModifier))
        addVarOverride(VD, Reason, dynamicLookupInfo);
      return;
    }

    if (auto *SD = dyn_cast<SubscriptDecl>(D)) {
      if (!hasIntroducer && !hasInitializerModifier)
        addSubscriptOverride(SD, Reason, dynamicLookupInfo);
    }

    if (auto *CD = dyn_cast<ConstructorDecl>(D)) {
      if (!isa<ProtocolDecl>(CD->getDeclContext()))
        return;
      if (hasIntroducer || hasOverride || hasOverridabilityModifier ||
          hasStaticOrClass)
        return;
      if (CD->isRequired() || CD->isDesignatedInit())
        addConstructor(CD, Reason, dynamicLookupInfo);
      return;
    }
  }

  void addDesignatedInitializers(NominalTypeDecl *NTD) {
    if (hasFuncIntroducer || hasVarIntroducer || hasTypealiasIntroducer ||
        hasOverridabilityModifier || hasStaticOrClass)
      return;

    const auto *CD = dyn_cast<ClassDecl>(NTD);
    if (!CD)
      return;
    if (!CD->hasSuperclass())
      return;
    CD = CD->getSuperclassDecl();
    for (const auto *Member : CD->getMembers()) {
      const auto *Constructor = dyn_cast<ConstructorDecl>(Member);
      if (!Constructor)
        continue;
      if (Constructor->hasStubImplementation())
        continue;
      if (Constructor->isDesignatedInit())
        addConstructor(Constructor, DeclVisibilityKind::MemberOfSuper, {});
    }
  }

  void addAssociatedTypes(NominalTypeDecl *NTD) {
    if (!hasTypealiasIntroducer &&
        (hasFuncIntroducer || hasVarIntroducer || hasInitializerModifier ||
         hasOverride || hasOverridabilityModifier || hasStaticOrClass))
      return;

    for (auto Conformance : NTD->getAllConformances()) {
      auto Proto = Conformance->getProtocol();
      if (!Proto->isAccessibleFrom(CurrDeclContext))
        continue;
      for (auto *ATD : Proto->getAssociatedTypeMembers()) {
        // FIXME: Also exclude the type alias that has already been specified.
        if (!Conformance->hasTypeWitness(ATD) ||
            ATD->hasDefaultDefinitionType())
          continue;
        addTypeAlias(
            ATD,
            DeclVisibilityKind::MemberOfProtocolConformedToByCurrentNominal,
            {});
      }
    }
  }

  static StringRef getResultBuilderDocComment(
      ResultBuilderBuildFunction function) {
    switch (function) {
    case ResultBuilderBuildFunction::BuildArray:
      return "Enables support for..in loops in a result builder by "
        "combining the results of all iterations into a single result";

    case ResultBuilderBuildFunction::BuildBlock:
      return "Required by every result builder to build combined results "
          "from statement blocks";

    case ResultBuilderBuildFunction::BuildEitherFirst:
      return "With buildEither(second:), enables support for 'if-else' and "
          "'switch' statements by folding conditional results into a single "
          "result";

    case ResultBuilderBuildFunction::BuildEitherSecond:
      return "With buildEither(first:), enables support for 'if-else' and "
          "'switch' statements by folding conditional results into a single "
          "result";

    case ResultBuilderBuildFunction::BuildExpression:
      return "If declared, provides contextual type information for statement "
          "expressions to translate them into partial results";

    case ResultBuilderBuildFunction::BuildFinalResult:
      return "If declared, this will be called on the partial result from the "
          "outermost block statement to produce the final returned result";

    case ResultBuilderBuildFunction::BuildLimitedAvailability:
      return "If declared, this will be called on the partial result of "
        "an 'if #available' block to allow the result builder to erase "
        "type information";

    case ResultBuilderBuildFunction::BuildOptional:
      return "Enables support for `if` statements that do not have an `else`";
    }
  }

  void addResultBuilderBuildCompletion(
      NominalTypeDecl *builder, Type componentType,
      ResultBuilderBuildFunction function) {
    CodeCompletionResultBuilder Builder(
        Sink,
        CodeCompletionResult::ResultKind::Pattern,
        SemanticContextKind::CurrentNominal, {});
    Builder.setExpectedTypeRelation(
        CodeCompletionResult::ExpectedTypeRelation::NotApplicable);

    if (!hasFuncIntroducer) {
      if (!hasAccessModifier &&
          builder->getFormalAccess() >= AccessLevel::Public)
        Builder.addAccessControlKeyword(AccessLevel::Public);

      if (!hasStaticOrClass)
        Builder.addTextChunk("static ");

      Builder.addTextChunk("func ");
    }

    std::string declStringWithoutFunc;
    {
      llvm::raw_string_ostream out(declStringWithoutFunc);
      printResultBuilderBuildFunction(
          builder, componentType, function, None, out);
    }
    Builder.addTextChunk(declStringWithoutFunc);
    Builder.addBraceStmtWithCursor();
    Builder.setBriefDocComment(getResultBuilderDocComment(function));
  }

  /// Add completions for the various "build" functions in a result builder.
  void addResultBuilderBuildCompletions(NominalTypeDecl *builder) {
    Type componentType = inferResultBuilderComponentType(builder);

    addResultBuilderBuildCompletion(
        builder, componentType, ResultBuilderBuildFunction::BuildBlock);
    addResultBuilderBuildCompletion(
        builder, componentType, ResultBuilderBuildFunction::BuildExpression);
    addResultBuilderBuildCompletion(
        builder, componentType, ResultBuilderBuildFunction::BuildOptional);
    addResultBuilderBuildCompletion(
        builder, componentType, ResultBuilderBuildFunction::BuildEitherFirst);
    addResultBuilderBuildCompletion(
        builder, componentType, ResultBuilderBuildFunction::BuildEitherSecond);
    addResultBuilderBuildCompletion(
        builder, componentType, ResultBuilderBuildFunction::BuildArray);
    addResultBuilderBuildCompletion(
        builder, componentType,
        ResultBuilderBuildFunction::BuildLimitedAvailability);
    addResultBuilderBuildCompletion(
        builder, componentType, ResultBuilderBuildFunction::BuildFinalResult);
  }

  void getOverrideCompletions(SourceLoc Loc) {
    if (!CurrDeclContext->isTypeContext())
      return;
    if (isa<ProtocolDecl>(CurrDeclContext))
      return;

    Type CurrTy = CurrDeclContext->getSelfTypeInContext();
    auto *NTD = CurrDeclContext->getSelfNominalTypeDecl();
    if (CurrTy && !CurrTy->is<ErrorType>()) {
      // Look for overridable static members too.
      Type Meta = MetatypeType::get(CurrTy);
      lookupVisibleMemberDecls(*this, Meta, CurrDeclContext,
                               /*includeInstanceMembers=*/true,
                               /*includeDerivedRequirements*/true,
                               /*includeProtocolExtensionMembers*/false);
      addDesignatedInitializers(NTD);
      addAssociatedTypes(NTD);
    }

    if (NTD && NTD->getAttrs().hasAttribute<ResultBuilderAttr>()) {
      addResultBuilderBuildCompletions(NTD);
    }
  }
};
} // end anonymous namespace

static void addSelectorModifierKeywords(CodeCompletionResultSink &sink) {
  auto addKeyword = [&](StringRef Name, CodeCompletionKeywordKind Kind) {
    CodeCompletionResultBuilder Builder(
                                  sink,
                                  CodeCompletionResult::ResultKind::Keyword,
                                  SemanticContextKind::None, {});
    Builder.setKeywordKind(Kind);
    Builder.addTextChunk(Name);
    Builder.addCallParameterColon();
    Builder.addSimpleTypedParameter("@objc property", /*IsVarArg=*/false);
  };

  addKeyword("getter", CodeCompletionKeywordKind::None);
  addKeyword("setter", CodeCompletionKeywordKind::None);
}

void CodeCompletionCallbacksImpl::completeDotExpr(CodeCompletionExpr *E,
                                                  SourceLoc DotLoc) {
  assert(P.Tok.is(tok::code_complete));

  // Don't produce any results in an enum element.
  if (InEnumElementRawValue)
    return;

  Kind = CompletionKind::DotExpr;
  if (ParseExprSelectorContext != ObjCSelectorContext::None) {
    PreferFunctionReferencesToCalls = true;
    CompleteExprSelectorContext = ParseExprSelectorContext;
  }

  ParsedExpr = E->getBase();
  this->DotLoc = DotLoc;
  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = E;
}

void CodeCompletionCallbacksImpl::completeStmtOrExpr(CodeCompletionExpr *E) {
  assert(P.Tok.is(tok::code_complete));
  Kind = CompletionKind::StmtOrExpr;
  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = E;
}

void CodeCompletionCallbacksImpl::completePostfixExprBeginning(CodeCompletionExpr *E) {
  assert(P.Tok.is(tok::code_complete));

  // Don't produce any results in an enum element.
  if (InEnumElementRawValue)
    return;

  Kind = CompletionKind::PostfixExprBeginning;
  if (ParseExprSelectorContext != ObjCSelectorContext::None) {
    PreferFunctionReferencesToCalls = true;
    CompleteExprSelectorContext = ParseExprSelectorContext;
    if (CompleteExprSelectorContext == ObjCSelectorContext::MethodSelector) {
      addSelectorModifierKeywords(CompletionContext.getResultSink());
    }
  }


  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = E;
}

void CodeCompletionCallbacksImpl::completeForEachSequenceBeginning(
    CodeCompletionExpr *E) {
  assert(P.Tok.is(tok::code_complete));
  Kind = CompletionKind::ForEachSequence;
  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = E;
}

void CodeCompletionCallbacksImpl::completePostfixExpr(Expr *E, bool hasSpace) {
  assert(P.Tok.is(tok::code_complete));

  // Don't produce any results in an enum element.
  if (InEnumElementRawValue)
    return;

  HasSpace = hasSpace;
  Kind = CompletionKind::PostfixExpr;
  if (ParseExprSelectorContext != ObjCSelectorContext::None) {
    PreferFunctionReferencesToCalls = true;
    CompleteExprSelectorContext = ParseExprSelectorContext;
  }

  ParsedExpr = E;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completePostfixExprParen(Expr *E,
                                                           Expr *CodeCompletionE) {
  assert(P.Tok.is(tok::code_complete));

  // Don't produce any results in an enum element.
  if (InEnumElementRawValue)
    return;

  Kind = CompletionKind::PostfixExprParen;
  ParsedExpr = E;
  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = static_cast<CodeCompletionExpr*>(CodeCompletionE);

  ShouldCompleteCallPatternAfterParen = true;
  if (Context.LangOpts.CodeCompleteCallPatternHeuristics) {
    // Lookahead one token to decide what kind of call completions to provide.
    // When it appears that there is already code for the call present, just
    // complete values and/or argument labels.  Otherwise give the entire call
    // pattern.
    Token next = P.peekToken();
    if (!next.isAtStartOfLine() && !next.is(tok::eof) && !next.is(tok::r_paren)) {
      ShouldCompleteCallPatternAfterParen = false;
    }
  }
}

void CodeCompletionCallbacksImpl::completeExprKeyPath(KeyPathExpr *KPE,
                                                      SourceLoc DotLoc) {
  Kind = (!KPE || KPE->isObjC()) ? CompletionKind::KeyPathExprObjC
                                 : CompletionKind::KeyPathExprSwift;
  ParsedExpr = KPE;
  this->DotLoc = DotLoc;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completePoundAvailablePlatform() {
  Kind = CompletionKind::PoundAvailablePlatform;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeTypeDeclResultBeginning() {
  Kind = CompletionKind::TypeDeclResultBeginning;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeTypeSimpleBeginning() {
  Kind = CompletionKind::TypeSimpleBeginning;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeDeclAttrParam(DeclAttrKind DK,
                                                        int Index) {
  Kind = CompletionKind::AttributeDeclParen;
  AttrKind = DK;
  AttrParamIndex = Index;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeEffectsSpecifier(bool hasAsync,
                                                           bool hasThrows) {
  Kind = CompletionKind::EffectsSpecifier;
  CurDeclContext = P.CurDeclContext;
  ParsedKeywords.clear();
  if (hasAsync)
    ParsedKeywords.emplace_back("async");
  if (hasThrows)
    ParsedKeywords.emplace_back("throws");
}

void CodeCompletionCallbacksImpl::completeDeclAttrBeginning(
    bool Sil, bool isIndependent) {
  Kind = CompletionKind::AttributeBegin;
  IsInSil = Sil;
  CurDeclContext = P.CurDeclContext;
  AttTargetIsIndependent = isIndependent;
}

void CodeCompletionCallbacksImpl::completeInPrecedenceGroup(SyntaxKind SK) {
  assert(P.Tok.is(tok::code_complete));

  SyntxKind = SK;
  Kind = CompletionKind::PrecedenceGroup;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeTypeIdentifierWithDot(
    IdentTypeRepr *ITR) {
  if (!ITR) {
    completeTypeSimpleBeginning();
    return;
  }
  Kind = CompletionKind::TypeIdentifierWithDot;
  ParsedTypeLoc = TypeLoc(ITR);
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeTypeIdentifierWithoutDot(
    IdentTypeRepr *ITR) {
  assert(ITR);
  Kind = CompletionKind::TypeIdentifierWithoutDot;
  ParsedTypeLoc = TypeLoc(ITR);
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeCaseStmtKeyword() {
  Kind = CompletionKind::CaseStmtKeyword;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeCaseStmtBeginning(CodeCompletionExpr *E) {
  assert(!InEnumElementRawValue);

  Kind = CompletionKind::CaseStmtBeginning;
  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = E;
}

void CodeCompletionCallbacksImpl::completeImportDecl(
    ImportPath::Builder &Path) {
  Kind = CompletionKind::Import;
  CurDeclContext = P.CurDeclContext;
  DotLoc = Path.empty() ? SourceLoc() : Path.back().Loc;
  if (DotLoc.isInvalid())
    return;
  auto Importer = static_cast<ClangImporter *>(CurDeclContext->getASTContext().
                                               getClangModuleLoader());
  std::vector<std::string> SubNames;
  Importer->collectSubModuleNames(Path.get().getModulePath(false), SubNames);
  ASTContext &Ctx = CurDeclContext->getASTContext();
  Path.push_back(Identifier());
  for (StringRef Sub : SubNames) {
    Path.back().Item = Ctx.getIdentifier(Sub);
    SubModuleNameVisibilityPairs.push_back(
      std::make_pair(Sub.str(),
                     Ctx.getLoadedModule(Path.get().getModulePath(false))));
  }
  Path.pop_back();
}

void CodeCompletionCallbacksImpl::completeUnresolvedMember(CodeCompletionExpr *E,
    SourceLoc DotLoc) {
  Kind = CompletionKind::UnresolvedMember;
  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = E;
  this->DotLoc = DotLoc;
}

void CodeCompletionCallbacksImpl::completeCallArg(CodeCompletionExpr *E,
                                                  bool isFirst) {
  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = E;
  Kind = CompletionKind::CallArg;

  ShouldCompleteCallPatternAfterParen = false;
  if (isFirst) {
    ShouldCompleteCallPatternAfterParen = true;
    if (Context.LangOpts.CodeCompleteCallPatternHeuristics) {
      // Lookahead one token to decide what kind of call completions to provide.
      // When it appears that there is already code for the call present, just
      // complete values and/or argument labels.  Otherwise give the entire call
      // pattern.
      Token next = P.peekToken();
      if (!next.isAtStartOfLine() && !next.is(tok::eof) && !next.is(tok::r_paren)) {
        ShouldCompleteCallPatternAfterParen = false;
      }
    }
  }
}

void CodeCompletionCallbacksImpl::completeLabeledTrailingClosure(
    CodeCompletionExpr *E, bool isAtStartOfLine) {
  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = E;
  Kind = CompletionKind::LabeledTrailingClosure;
  IsAtStartOfLine = isAtStartOfLine;
}

void CodeCompletionCallbacksImpl::completeReturnStmt(CodeCompletionExpr *E) {
  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = E;
  Kind = CompletionKind::ReturnStmtExpr;
}

void CodeCompletionCallbacksImpl::completeYieldStmt(CodeCompletionExpr *E,
                                                    Optional<unsigned> index) {
  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = E;
  // TODO: use a different completion kind when completing without an index
  // in a multiple-value context.
  Kind = CompletionKind::YieldStmtExpr;
}

void CodeCompletionCallbacksImpl::completeAfterPoundExpr(
    CodeCompletionExpr *E, Optional<StmtKind> ParentKind) {
  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = E;
  Kind = CompletionKind::AfterPoundExpr;
  ParentStmtKind = ParentKind;
}

void CodeCompletionCallbacksImpl::completeAfterPoundDirective() {
  CurDeclContext = P.CurDeclContext;
  Kind = CompletionKind::AfterPoundDirective;
}

void CodeCompletionCallbacksImpl::completePlatformCondition() {
  CurDeclContext = P.CurDeclContext;
  Kind = CompletionKind::PlatformConditon;
}

void CodeCompletionCallbacksImpl::completeAfterIfStmt(bool hasElse) {
  CurDeclContext = P.CurDeclContext;
  if (hasElse) {
    Kind = CompletionKind::AfterIfStmtElse;
  } else {
    Kind = CompletionKind::StmtOrExpr;
  }
}

void CodeCompletionCallbacksImpl::completeGenericRequirement() {
  CurDeclContext = P.CurDeclContext;
  Kind = CompletionKind::GenericRequirement;
}

void CodeCompletionCallbacksImpl::completeNominalMemberBeginning(
    SmallVectorImpl<StringRef> &Keywords, SourceLoc introducerLoc) {
  assert(!InEnumElementRawValue);
  this->introducerLoc = introducerLoc;
  ParsedKeywords.clear();
  ParsedKeywords.append(Keywords.begin(), Keywords.end());
  Kind = CompletionKind::NominalMemberBeginning;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeAccessorBeginning(
    CodeCompletionExpr *E) {
  Kind = CompletionKind::AccessorBeginning;
  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = E;
}

void CodeCompletionCallbacksImpl::completeStmtLabel(StmtKind ParentKind) {
  CurDeclContext = P.CurDeclContext;
  Kind = CompletionKind::StmtLabel;
  ParentStmtKind = ParentKind;
}

void CodeCompletionCallbacksImpl::completeForEachPatternBeginning(
    bool hasTry, bool hasAwait) {
  CurDeclContext = P.CurDeclContext;
  Kind = CompletionKind::ForEachPatternBeginning;
  ParsedKeywords.clear();
  if (hasTry)
    ParsedKeywords.emplace_back("try");
  if (hasAwait)
    ParsedKeywords.emplace_back("await");
}

void CodeCompletionCallbacksImpl::completeTypeAttrBeginning() {
  CurDeclContext = P.CurDeclContext;
  Kind = CompletionKind::TypeAttrBeginning;
}

static bool isDynamicLookup(Type T) {
  return T->getRValueType()->isAnyObject();
}

static bool isClangSubModule(ModuleDecl *TheModule) {
  if (auto ClangMod = TheModule->findUnderlyingClangModule())
    return ClangMod->isSubModule();
  return false;
}

static void
addKeyword(CodeCompletionResultSink &Sink, StringRef Name,
           CodeCompletionKeywordKind Kind, StringRef TypeAnnotation = "",
           CodeCompletionResult::ExpectedTypeRelation TypeRelation =
               CodeCompletionResult::ExpectedTypeRelation::NotApplicable,
           CodeCompletionFlair Flair = {}) {
  CodeCompletionResultBuilder Builder(Sink,
                                      CodeCompletionResult::ResultKind::Keyword,
                                      SemanticContextKind::None, {});
  Builder.setKeywordKind(Kind);
  Builder.addKeyword(Name);
  Builder.addFlair(Flair);
  if (!TypeAnnotation.empty())
    Builder.addTypeAnnotation(TypeAnnotation);
  Builder.setExpectedTypeRelation(TypeRelation);
}

static void addDeclKeywords(CodeCompletionResultSink &Sink, DeclContext *DC,
                            bool IsConcurrencyEnabled,
                            bool IsDistributedEnabled) {
  auto isTypeDeclIntroducer = [](CodeCompletionKeywordKind Kind,
                                 Optional<DeclAttrKind> DAK) -> bool {
    switch (Kind) {
    case CodeCompletionKeywordKind::kw_protocol:
    case CodeCompletionKeywordKind::kw_class:
    case CodeCompletionKeywordKind::kw_struct:
    case CodeCompletionKeywordKind::kw_enum:
    case CodeCompletionKeywordKind::kw_extension:
      return true;
    case CodeCompletionKeywordKind::None:
      if (DAK && *DAK == DeclAttrKind::DAK_Actor) {
        return true;
      }
      break;
    default:
      break;
    }
    return false;
  };
  auto isTopLevelOnlyDeclIntroducer = [](CodeCompletionKeywordKind Kind,
                                         Optional<DeclAttrKind> DAK) -> bool {
    switch (Kind) {
    case CodeCompletionKeywordKind::kw_operator:
    case CodeCompletionKeywordKind::kw_precedencegroup:
    case CodeCompletionKeywordKind::kw_import:
    case CodeCompletionKeywordKind::kw_protocol:
    case CodeCompletionKeywordKind::kw_extension:
      return true;
    default:
      return false;
    }
  };

  auto getFlair = [&](CodeCompletionKeywordKind Kind,
                      Optional<DeclAttrKind> DAK) -> CodeCompletionFlair {
    if (isCodeCompletionAtTopLevelOfLibraryFile(DC)) {
      // Type decls are common in library file top-level.
      if (isTypeDeclIntroducer(Kind, DAK))
        return CodeCompletionFlairBit::CommonKeywordAtCurrentPosition;
    }
    if (isa<ProtocolDecl>(DC)) {
      // Protocols cannot have nested type decls (other than 'typealias').
      if (isTypeDeclIntroducer(Kind, DAK))
        return CodeCompletionFlairBit::RareKeywordAtCurrentPosition;
    }
    if (DC->isTypeContext()) {
      // Top-level only decls are invalid in type context.
      if (isTopLevelOnlyDeclIntroducer(Kind, DAK))
        return CodeCompletionFlairBit::RareKeywordAtCurrentPosition;
    }
    if (isCompletionDeclContextLocalContext(DC)) {
      // Local type decl are valid, but not common.
      if (isTypeDeclIntroducer(Kind, DAK))
        return CodeCompletionFlairBit::RareKeywordAtCurrentPosition;

      // Top-level only decls are invalid in function body.
      if (isTopLevelOnlyDeclIntroducer(Kind, DAK))
        return CodeCompletionFlairBit::RareKeywordAtCurrentPosition;

      // 'init', 'deinit' and 'subscript' are invalid in function body.
      // Access control modifiers are invalid in function body.
      switch (Kind) {
      case CodeCompletionKeywordKind::kw_init:
      case CodeCompletionKeywordKind::kw_deinit:
      case CodeCompletionKeywordKind::kw_subscript:
      case CodeCompletionKeywordKind::kw_private:
      case CodeCompletionKeywordKind::kw_fileprivate:
      case CodeCompletionKeywordKind::kw_internal:
      case CodeCompletionKeywordKind::kw_public:
      case CodeCompletionKeywordKind::kw_static:
        return CodeCompletionFlairBit::RareKeywordAtCurrentPosition;

      default:
        break;
      }

      // These modifiers are invalid for decls in function body.
      if (DAK) {
        switch (*DAK) {
        case DeclAttrKind::DAK_Lazy:
        case DeclAttrKind::DAK_Final:
        case DeclAttrKind::DAK_Infix:
        case DeclAttrKind::DAK_Frozen:
        case DeclAttrKind::DAK_Prefix:
        case DeclAttrKind::DAK_Postfix:
        case DeclAttrKind::DAK_Dynamic:
        case DeclAttrKind::DAK_Override:
        case DeclAttrKind::DAK_Optional:
        case DeclAttrKind::DAK_Required:
        case DeclAttrKind::DAK_Convenience:
        case DeclAttrKind::DAK_AccessControl:
        case DeclAttrKind::DAK_Nonisolated:
          return CodeCompletionFlairBit::RareKeywordAtCurrentPosition;

        default:
          break;
        }
      }
    }
    return None;
  };

  auto AddDeclKeyword = [&](StringRef Name, CodeCompletionKeywordKind Kind,
                            Optional<DeclAttrKind> DAK) {
    if (Name == "let" || Name == "var") {
      // Treat keywords that could be the start of a pattern specially.
      return;
    }

    // FIXME: This should use canUseAttributeOnDecl.

    // Remove user inaccessible keywords.
    if (DAK.hasValue() && DeclAttribute::isUserInaccessible(*DAK))
      return;

    // Remove keywords only available when concurrency is enabled.
    if (DAK.hasValue() && !IsConcurrencyEnabled &&
        DeclAttribute::isConcurrencyOnly(*DAK))
      return;

    // Remove keywords only available when distributed is enabled.
    if (DAK.hasValue() && !IsDistributedEnabled &&
        DeclAttribute::isDistributedOnly(*DAK))
      return;

    addKeyword(
        Sink, Name, Kind, /*TypeAnnotation=*/"",
        /*TypeRelation=*/CodeCompletionResult::ExpectedTypeRelation::NotApplicable,
        getFlair(Kind, DAK));
  };

#define DECL_KEYWORD(kw)                                                       \
  AddDeclKeyword(#kw, CodeCompletionKeywordKind::kw_##kw, None);
#include "swift/Syntax/TokenKinds.def"

  // Context-sensitive keywords.
  auto AddCSKeyword = [&](StringRef Name, DeclAttrKind Kind) {
    AddDeclKeyword(Name, CodeCompletionKeywordKind::None, Kind);
  };

#define CONTEXTUAL_CASE(KW, CLASS) AddCSKeyword(#KW, DAK_##CLASS);
#define CONTEXTUAL_DECL_ATTR(KW, CLASS, ...) CONTEXTUAL_CASE(KW, CLASS)
#define CONTEXTUAL_DECL_ATTR_ALIAS(KW, CLASS) CONTEXTUAL_CASE(KW, CLASS)
#define CONTEXTUAL_SIMPLE_DECL_ATTR(KW, CLASS, ...) CONTEXTUAL_CASE(KW, CLASS)
#include <swift/AST/Attr.def>
#undef CONTEXTUAL_CASE
}

static void addStmtKeywords(CodeCompletionResultSink &Sink, DeclContext *DC,
                            bool MaybeFuncBody) {
  CodeCompletionFlair flair;
  // Starting a statement at top-level in non-script files is invalid.
  if (isCodeCompletionAtTopLevelOfLibraryFile(DC)) {
    flair |= CodeCompletionFlairBit::ExpressionAtNonScriptOrMainFileScope;
  }

  auto AddStmtKeyword = [&](StringRef Name, CodeCompletionKeywordKind Kind) {
    if (!MaybeFuncBody && Kind == CodeCompletionKeywordKind::kw_return)
      return;
    addKeyword(Sink, Name, Kind, "",
               CodeCompletionResult::ExpectedTypeRelation::NotApplicable,
               flair);
  };
#define STMT_KEYWORD(kw) AddStmtKeyword(#kw, CodeCompletionKeywordKind::kw_##kw);
#include "swift/Syntax/TokenKinds.def"
}

static void addCaseStmtKeywords(CodeCompletionResultSink &Sink) {
  addKeyword(Sink, "case", CodeCompletionKeywordKind::kw_case);
  addKeyword(Sink, "default", CodeCompletionKeywordKind::kw_default);
}

static void addLetVarKeywords(CodeCompletionResultSink &Sink) {
  addKeyword(Sink, "let", CodeCompletionKeywordKind::kw_let);
  addKeyword(Sink, "var", CodeCompletionKeywordKind::kw_var);
}

static void addAccessorKeywords(CodeCompletionResultSink &Sink) {
  addKeyword(Sink, "get", CodeCompletionKeywordKind::None);
  addKeyword(Sink, "set", CodeCompletionKeywordKind::None);
}

static void addObserverKeywords(CodeCompletionResultSink &Sink) {
  addKeyword(Sink, "willSet", CodeCompletionKeywordKind::None);
  addKeyword(Sink, "didSet", CodeCompletionKeywordKind::None);
}

static void addExprKeywords(CodeCompletionResultSink &Sink, DeclContext *DC) {
  // Expression is invalid at top-level of non-script files.
  CodeCompletionFlair flair;
  if (isCodeCompletionAtTopLevelOfLibraryFile(DC)) {
    flair |= CodeCompletionFlairBit::ExpressionAtNonScriptOrMainFileScope;
  }

  // Expr keywords.
  addKeyword(Sink, "try", CodeCompletionKeywordKind::kw_try, "",
             CodeCompletionResult::ExpectedTypeRelation::NotApplicable, flair);
  addKeyword(Sink, "try!", CodeCompletionKeywordKind::kw_try, "",
             CodeCompletionResult::ExpectedTypeRelation::NotApplicable, flair);
  addKeyword(Sink, "try?", CodeCompletionKeywordKind::kw_try, "",
             CodeCompletionResult::ExpectedTypeRelation::NotApplicable, flair);
  addKeyword(Sink, "await", CodeCompletionKeywordKind::None, "",
             CodeCompletionResult::ExpectedTypeRelation::NotApplicable, flair);
}

static void addSuperKeyword(CodeCompletionResultSink &Sink, DeclContext *DC) {
  if (!DC)
    return;
  auto *TC = DC->getInnermostTypeContext();
  if (!TC)
    return;
  auto *CD = TC->getSelfClassDecl();
  if (!CD)
    return;
  Type ST = CD->getSuperclass();
  if (ST.isNull() || ST->is<ErrorType>())
    return;

  CodeCompletionResultBuilder Builder(Sink,
                                      CodeCompletionResult::ResultKind::Keyword,
                                      SemanticContextKind::CurrentNominal,
                                      {});
  if (auto *AFD = dyn_cast<AbstractFunctionDecl>(DC)) {
    if (AFD->getOverriddenDecl() != nullptr) {
      Builder.addFlair(CodeCompletionFlairBit::CommonKeywordAtCurrentPosition);
    }
  }

  Builder.setKeywordKind(CodeCompletionKeywordKind::kw_super);
  Builder.addKeyword("super");
  Builder.addTypeAnnotation(ST, PrintOptions());
}

static void addOpaqueTypeKeyword(CodeCompletionResultSink &Sink) {
  addKeyword(Sink, "some", CodeCompletionKeywordKind::None, "some");
}

static void addAnyTypeKeyword(CodeCompletionResultSink &Sink, Type T) {
  CodeCompletionResultBuilder Builder(Sink,
                                      CodeCompletionResult::ResultKind::Keyword,
                                      SemanticContextKind::None, {});
  Builder.setKeywordKind(CodeCompletionKeywordKind::None);
  Builder.addKeyword("Any");
  Builder.addTypeAnnotation(T, PrintOptions());
}

void CodeCompletionCallbacksImpl::addKeywords(CodeCompletionResultSink &Sink,
                                              bool MaybeFuncBody) {
  switch (Kind) {
  case CompletionKind::None:
  case CompletionKind::DotExpr:
  case CompletionKind::AttributeDeclParen:
  case CompletionKind::AttributeBegin:
  case CompletionKind::PoundAvailablePlatform:
  case CompletionKind::Import:
  case CompletionKind::UnresolvedMember:
  case CompletionKind::LabeledTrailingClosure:
  case CompletionKind::AfterPoundExpr:
  case CompletionKind::AfterPoundDirective:
  case CompletionKind::PlatformConditon:
  case CompletionKind::GenericRequirement:
  case CompletionKind::KeyPathExprObjC:
  case CompletionKind::KeyPathExprSwift:
  case CompletionKind::PrecedenceGroup:
  case CompletionKind::StmtLabel:
  case CompletionKind::TypeAttrBeginning:
    break;

  case CompletionKind::EffectsSpecifier: {
    if (!llvm::is_contained(ParsedKeywords, "async"))
      addKeyword(Sink, "async", CodeCompletionKeywordKind::None);
    if (!llvm::is_contained(ParsedKeywords, "throws"))
      addKeyword(Sink, "throws", CodeCompletionKeywordKind::kw_throws);
    break;
  }

  case CompletionKind::AccessorBeginning: {
    // TODO: Omit already declared or mutally exclusive accessors.
    //       E.g. If 'get' is already declared, emit 'set' only.
    addAccessorKeywords(Sink);

    // Only 'var' for non-protocol context can have 'willSet' and 'didSet'.
    assert(ParsedDecl);
    VarDecl *var = dyn_cast<VarDecl>(ParsedDecl);
    if (auto accessor = dyn_cast<AccessorDecl>(ParsedDecl))
      var = dyn_cast<VarDecl>(accessor->getStorage());
    if (var && !var->getDeclContext()->getSelfProtocolDecl())
      addObserverKeywords(Sink);

    if (!isa<AccessorDecl>(ParsedDecl))
      break;

    MaybeFuncBody = true;
    LLVM_FALLTHROUGH;
  }
  case CompletionKind::StmtOrExpr:
    addDeclKeywords(Sink, CurDeclContext,
                    Context.LangOpts.EnableExperimentalConcurrency,
                    Context.LangOpts.EnableExperimentalDistributed);
    addStmtKeywords(Sink, CurDeclContext, MaybeFuncBody);
    LLVM_FALLTHROUGH;
  case CompletionKind::ReturnStmtExpr:
  case CompletionKind::YieldStmtExpr:
  case CompletionKind::PostfixExprBeginning:
  case CompletionKind::ForEachSequence:
    addSuperKeyword(Sink, CurDeclContext);
    addLetVarKeywords(Sink);
    addExprKeywords(Sink, CurDeclContext);
    addAnyTypeKeyword(Sink, CurDeclContext->getASTContext().TheAnyType);
    break;

  case CompletionKind::CallArg:
  case CompletionKind::PostfixExprParen:
    // Note that we don't add keywords here as the completion might be for
    // an argument list pattern. We instead add keywords later in
    // CodeCompletionCallbacksImpl::doneParsing when we know we're not
    // completing for a argument list pattern.
    break;

  case CompletionKind::CaseStmtKeyword:
    addCaseStmtKeywords(Sink);
    break;

  case CompletionKind::PostfixExpr:
  case CompletionKind::CaseStmtBeginning:
  case CompletionKind::TypeIdentifierWithDot:
  case CompletionKind::TypeIdentifierWithoutDot:
    break;

  case CompletionKind::TypeDeclResultBeginning: {
    auto DC = CurDeclContext;
    if (ParsedDecl && ParsedDecl == CurDeclContext->getAsDecl())
      DC = ParsedDecl->getDeclContext();
    if (!isa<ProtocolDecl>(DC))
      if (DC->isTypeContext() || (ParsedDecl && isa<FuncDecl>(ParsedDecl)))
        addOpaqueTypeKeyword(Sink);

    LLVM_FALLTHROUGH;
  }
  case CompletionKind::TypeSimpleBeginning:
    addAnyTypeKeyword(Sink, CurDeclContext->getASTContext().TheAnyType);
    break;

  case CompletionKind::NominalMemberBeginning: {
    bool HasDeclIntroducer = llvm::find_if(ParsedKeywords,
                                           [this](const StringRef kw) {
      return llvm::StringSwitch<bool>(kw)
        .Case("associatedtype", true)
        .Case("class", !CurDeclContext || !isa<ClassDecl>(CurDeclContext))
        .Case("deinit", true)
        .Case("enum", true)
        .Case("extension", true)
        .Case("func", true)
        .Case("import", true)
        .Case("init", true)
        .Case("let", true)
        .Case("operator", true)
        .Case("precedencegroup", true)
        .Case("protocol", true)
        .Case("struct", true)
        .Case("subscript", true)
        .Case("typealias", true)
        .Case("var", true)
        .Default(false);
    }) != ParsedKeywords.end();
    if (!HasDeclIntroducer) {
      addDeclKeywords(Sink, CurDeclContext,
                      Context.LangOpts.EnableExperimentalConcurrency,
                      Context.LangOpts.EnableExperimentalDistributed);
      addLetVarKeywords(Sink);
    }
    break;
  }

  case CompletionKind::AfterIfStmtElse:
    addKeyword(Sink, "if", CodeCompletionKeywordKind::kw_if);
    break;
  case CompletionKind::ForEachPatternBeginning:
    if (!llvm::is_contained(ParsedKeywords, "try"))
      addKeyword(Sink, "try", CodeCompletionKeywordKind::kw_try);
    if (!llvm::is_contained(ParsedKeywords, "await"))
      addKeyword(Sink, "await", CodeCompletionKeywordKind::None);
    addKeyword(Sink, "var", CodeCompletionKeywordKind::kw_var);
    addKeyword(Sink, "case", CodeCompletionKeywordKind::kw_case);
  }
}

static void addPoundDirectives(CodeCompletionResultSink &Sink) {
  auto addWithName =
      [&](StringRef name, CodeCompletionKeywordKind K,
          llvm::function_ref<void(CodeCompletionResultBuilder &)> consumer =
              nullptr) {
        CodeCompletionResultBuilder Builder(Sink, CodeCompletionResult::Keyword,
                                            SemanticContextKind::None, {});
        Builder.addBaseName(name);
        Builder.setKeywordKind(K);
        if (consumer)
          consumer(Builder);
      };

  addWithName("sourceLocation", CodeCompletionKeywordKind::pound_sourceLocation,
              [&] (CodeCompletionResultBuilder &Builder) {
    Builder.addLeftParen();
    Builder.addTextChunk("file");
    Builder.addCallParameterColon();
    Builder.addSimpleTypedParameter("String");
    Builder.addComma();
    Builder.addTextChunk("line");
    Builder.addCallParameterColon();
    Builder.addSimpleTypedParameter("Int");
    Builder.addRightParen();
  });
  addWithName("warning", CodeCompletionKeywordKind::pound_warning,
              [&] (CodeCompletionResultBuilder &Builder) {
    Builder.addLeftParen();
    Builder.addTextChunk("\"");
    Builder.addSimpleNamedParameter("message");
    Builder.addTextChunk("\"");
    Builder.addRightParen();
  });
  addWithName("error", CodeCompletionKeywordKind::pound_error,
              [&] (CodeCompletionResultBuilder &Builder) {
    Builder.addLeftParen();
    Builder.addTextChunk("\"");
    Builder.addSimpleNamedParameter("message");
    Builder.addTextChunk("\"");
    Builder.addRightParen();
  });

  addWithName("if ", CodeCompletionKeywordKind::pound_if,
              [&] (CodeCompletionResultBuilder &Builder) {
    Builder.addSimpleNamedParameter("condition");
  });

  // FIXME: These directives are only valid in conditional completion block.
  addWithName("elseif ", CodeCompletionKeywordKind::pound_elseif,
              [&] (CodeCompletionResultBuilder &Builder) {
    Builder.addSimpleNamedParameter("condition");
  });
  addWithName("else", CodeCompletionKeywordKind::pound_else);
  addWithName("endif", CodeCompletionKeywordKind::pound_endif);
}

/// Add platform conditions used in '#if' and '#elseif' directives.
static void addPlatformConditions(CodeCompletionResultSink &Sink) {
  auto addWithName =
      [&](StringRef Name,
          llvm::function_ref<void(CodeCompletionResultBuilder & Builder)>
              consumer) {
        CodeCompletionResultBuilder Builder(
            Sink, CodeCompletionResult::ResultKind::Pattern,
            // FIXME: SemanticContextKind::CurrentModule is not correct.
            // Use 'None' (and fix prioritization) or introduce a new context.
            SemanticContextKind::CurrentModule, {});
        Builder.addFlair(CodeCompletionFlairBit::ExpressionSpecific);
        Builder.addBaseName(Name);
        Builder.addLeftParen();
        consumer(Builder);
        Builder.addRightParen();
      };

  addWithName("os", [](CodeCompletionResultBuilder &Builder) {
    Builder.addSimpleNamedParameter("name");
  });
  addWithName("arch", [](CodeCompletionResultBuilder &Builder) {
    Builder.addSimpleNamedParameter("name");
  });
  addWithName("canImport", [](CodeCompletionResultBuilder &Builder) {
    Builder.addSimpleNamedParameter("module");
  });
  addWithName("targetEnvironment", [](CodeCompletionResultBuilder &Builder) {
    Builder.addTextChunk("simulator");
  });
  addWithName("swift", [](CodeCompletionResultBuilder &Builder) {
    Builder.addTextChunk(">=");
    Builder.addSimpleNamedParameter("version");
  });
  addWithName("swift", [](CodeCompletionResultBuilder &Builder) {
    Builder.addTextChunk("<");
    Builder.addSimpleNamedParameter("version");
  });
  addWithName("compiler", [](CodeCompletionResultBuilder &Builder) {
    Builder.addTextChunk(">=");
    Builder.addSimpleNamedParameter("version");
  });
  addWithName("compiler", [](CodeCompletionResultBuilder &Builder) {
    Builder.addTextChunk("<");
    Builder.addSimpleNamedParameter("version");
  });

  addKeyword(Sink, "true", CodeCompletionKeywordKind::kw_true, "Bool");
  addKeyword(Sink, "false", CodeCompletionKeywordKind::kw_false, "Bool");
}

/// Add flags specified by '-D' to completion results.
static void addConditionalCompilationFlags(ASTContext &Ctx,
                                           CodeCompletionResultSink &Sink) {
  for (auto Flag : Ctx.LangOpts.getCustomConditionalCompilationFlags()) {
    // TODO: Should we filter out some flags?
    CodeCompletionResultBuilder Builder(
        Sink, CodeCompletionResult::ResultKind::Keyword,
        // FIXME: SemanticContextKind::CurrentModule is not correct.
        // Use 'None' (and fix prioritization) or introduce a new context.
        SemanticContextKind::CurrentModule, {});
    Builder.addFlair(CodeCompletionFlairBit::ExpressionSpecific);
    Builder.addTextChunk(Flag);
  }
}

/// Add flairs to the each item in \p results .
///
/// If \p Sink is passed, the pointer of the each result may be replaced with a
/// pointer to the new item allocated in \p Sink.
/// If \p Sink is nullptr, the pointee of each result may be modified in place.
static void postProcessResults(MutableArrayRef<CodeCompletionResult *> results,
                               CompletionKind Kind, DeclContext *DC,
                               CodeCompletionResultSink *Sink) {
  for (CodeCompletionResult *&result : results) {
    bool modified = false;
    auto flair = result->getFlair();

    // Starting a statement with a protocol name is not common. So protocol
    // names at non-type name position are "rare".
    if (result->getKind() == CodeCompletionResult::ResultKind::Declaration &&
        result->getAssociatedDeclKind() == CodeCompletionDeclKind::Protocol &&
        Kind != CompletionKind::TypeSimpleBeginning &&
        Kind != CompletionKind::TypeIdentifierWithoutDot &&
        Kind != CompletionKind::TypeIdentifierWithDot &&
        Kind != CompletionKind::TypeDeclResultBeginning &&
        Kind != CompletionKind::GenericRequirement) {
      flair |= CodeCompletionFlairBit::RareTypeAtCurrentPosition;
      modified = true;
    }

    // Starting a statement at top-level in non-script files is invalid.
    if (Kind == CompletionKind::StmtOrExpr &&
        result->getKind() == CodeCompletionResult::ResultKind::Declaration &&
        isCodeCompletionAtTopLevelOfLibraryFile(DC)) {
      flair |= CodeCompletionFlairBit::ExpressionAtNonScriptOrMainFileScope;
      modified = true;
    }

    if (!modified)
      continue;

    if (Sink) {
      // Replace the result with a new result with the flair.
      result = result->withFlair(flair, *Sink);
    } else {
      // 'Sink' == nullptr means the result is modifiable in place.
      result->setFlair(flair);
    }
  }
}

static void deliverCompletionResults(CodeCompletionContext &CompletionContext,
                                     CompletionLookup &Lookup,
                                     DeclContext *DC,
                                     CodeCompletionConsumer &Consumer) {
  auto &SF = *DC->getParentSourceFile();
  llvm::SmallPtrSet<Identifier, 8> seenModuleNames;
  std::vector<RequestedCachedModule> RequestedModules;

  SmallPtrSet<ModuleDecl *, 4> explictlyImportedModules;
  {
    // Collect modules directly imported in this SourceFile.
    SmallVector<ImportedModule, 4> directImport;
    SF.getImportedModules(directImport,
                          {ModuleDecl::ImportFilterKind::Default,
                           ModuleDecl::ImportFilterKind::ImplementationOnly});
    for (auto import : directImport)
      explictlyImportedModules.insert(import.importedModule);

    // Exclude modules implicitly imported in the current module.
    auto implicitImports = SF.getParentModule()->getImplicitImports();
    for (auto import : implicitImports.imports)
      explictlyImportedModules.erase(import.module.importedModule);

    // Consider the current module "explicit".
    explictlyImportedModules.insert(SF.getParentModule());
  }

  for (auto &Request: Lookup.RequestedCachedResults) {
    llvm::DenseSet<CodeCompletionCache::Key> ImportsSeen;
    auto handleImport = [&](ImportedModule Import) {
      ModuleDecl *TheModule = Import.importedModule;
      ImportPath::Access Path = Import.accessPath;
      if (TheModule->getFiles().empty())
        return;

      // Clang submodules are ignored and there's no lookup cost involved,
      // so just ignore them and don't put the empty results in the cache
      // because putting a lot of objects in the cache will push out
      // other lookups.
      if (isClangSubModule(TheModule))
        return;

      std::vector<std::string> AccessPath;
      for (auto Piece : Path) {
        AccessPath.push_back(std::string(Piece.Item));
      }

      StringRef ModuleFilename = TheModule->getModuleFilename();
      // ModuleFilename can be empty if something strange happened during
      // module loading, for example, the module file is corrupted.
      if (!ModuleFilename.empty()) {
        auto &Ctx = TheModule->getASTContext();
        CodeCompletionCache::Key K{
            ModuleFilename.str(),
            std::string(TheModule->getName()),
            AccessPath,
            Request.NeedLeadingDot,
            SF.hasTestableOrPrivateImport(
                AccessLevel::Internal, TheModule,
                SourceFile::ImportQueryKind::TestableOnly),
            SF.hasTestableOrPrivateImport(
                AccessLevel::Internal, TheModule,
                SourceFile::ImportQueryKind::PrivateOnly),
            Ctx.LangOpts.CodeCompleteInitsInPostfixExpr,
            CompletionContext.getAnnotateResult(),
        };

        using PairType = llvm::DenseSet<swift::ide::CodeCompletionCache::Key,
            llvm::DenseMapInfo<CodeCompletionCache::Key>>::iterator;
        std::pair<PairType, bool> Result = ImportsSeen.insert(K);
        if (!Result.second)
          return; // already handled.
        RequestedModules.push_back({std::move(K), TheModule,
          Request.OnlyTypes, Request.OnlyPrecedenceGroups});

        auto TheModuleName = TheModule->getName();
        if (Request.IncludeModuleQualifier &&
            (!Lookup.isHiddenModuleName(TheModuleName) ||
             explictlyImportedModules.contains(TheModule)) &&
            seenModuleNames.insert(TheModuleName).second)
          Lookup.addModuleName(TheModule);
      }
    };

    if (Request.TheModule) {
      // FIXME: actually check imports.
      for (auto Import : namelookup::getAllImports(Request.TheModule)) {
        handleImport(Import);
      }
    } else {
      // Add results from current module.
      Lookup.getToplevelCompletions(Request.OnlyTypes);

      // Add the qualifying module name
      auto curModule = SF.getParentModule();
      if (Request.IncludeModuleQualifier &&
          seenModuleNames.insert(curModule->getName()).second)
        Lookup.addModuleName(curModule);

      // Add results for all imported modules.
      SmallVector<ImportedModule, 4> Imports;
      SF.getImportedModules(
          Imports, {ModuleDecl::ImportFilterKind::Exported,
                    ModuleDecl::ImportFilterKind::Default,
                    ModuleDecl::ImportFilterKind::ImplementationOnly});

      for (auto Imported : Imports) {
        for (auto Import : namelookup::getAllImports(Imported.importedModule))
          handleImport(Import);
      }
    }
  }
  Lookup.RequestedCachedResults.clear();
  CompletionContext.typeContextKind = Lookup.typeContextKind();

  postProcessResults(CompletionContext.getResultSink().Results,
                     CompletionContext.CodeCompletionKind, DC,
                     /*Sink=*/nullptr);

  Consumer.handleResultsAndModules(CompletionContext, RequestedModules, DC);
}

/// Populates a vector of parameters to suggest along with a vector of types to
/// match the lookup results against.
///
/// \Returns true if global lookup should be performed.
static bool addPossibleParams(
    const ArgumentTypeCheckCompletionCallback::Result &Ret,
    SmallVectorImpl<PossibleParamInfo> &Params, SmallVectorImpl<Type> &Types) {
  ArrayRef<AnyFunctionType::Param> ParamsToPass =
      Ret.FuncTy->getAs<AnyFunctionType>()->getParams();
  ParameterList *PL = nullptr;

  if (auto *FD = dyn_cast<AbstractFunctionDecl>(Ret.FuncD)) {
    PL = FD->getParameters();
  } else if (auto *SD = dyn_cast<SubscriptDecl>(Ret.FuncD)) {
    PL = SD->getIndices();
  } else {
    assert(false && "Unhandled decl type?");
    return true;
  }
  assert(PL->size() == ParamsToPass.size());

  if (!Ret.ParamIdx)
    return true;

  if (Ret.HasLabel) {
    Types.push_back(Ret.ExpectedType);
    return true;
  }

  bool showGlobalCompletions = false;
  for (auto Idx: range(*Ret.ParamIdx, PL->size())) {
    bool IsCompletion = Idx == Ret.ParamIdx;

    // Stop at the first param claimed by other arguments.
    if (!IsCompletion && llvm::is_contained(Ret.ClaimedParamIndices, Idx))
      break;

    const AnyFunctionType::Param *P = &ParamsToPass[Idx];
    bool Defaulted = PL->get(Idx)->isDefaultArgument() || PL->get(Idx)->isVariadic();
    PossibleParamInfo PP(P, Defaulted);
    if (llvm::any_of(Params, [&](PossibleParamInfo &Existing){ return PP == Existing; }))
      continue;

    if (P->hasLabel() && !(IsCompletion && Ret.IsNoninitialVariadic)) {
      Params.push_back(std::move(PP));
    } else {
      showGlobalCompletions = true;
      Types.push_back(P->getPlainType());
    }
    if (!Defaulted)
      break;
  }
  return showGlobalCompletions;
}

static void deliverArgumentResults(
    ArrayRef<ArgumentTypeCheckCompletionCallback::Result> Results,
    bool IncludeSignature, SourceLoc Loc, DeclContext *DC,
    ide::CodeCompletionContext &CompletionCtx,
    CodeCompletionConsumer &Consumer) {
  ASTContext &Ctx = DC->getASTContext();
  CompletionLookup Lookup(CompletionCtx.getResultSink(), Ctx, DC,
                          &CompletionCtx);

  bool shouldPerformGlobalCompletion = Results.empty();
  SmallVector<Type, 8> ExpectedTypes;

  if (IncludeSignature && !Results.empty()) {
    Lookup.setHaveLParen(true);
    for (auto &Result: Results) {
      auto SemanticContext = SemanticContextKind::None;
      NominalTypeDecl *BaseNominal = nullptr;
      Type BaseTy = Result.BaseType;
      if (BaseTy) {
        if (auto InstanceTy = BaseTy->getMetatypeInstanceType())
          BaseTy = InstanceTy;
        if ((BaseNominal = BaseTy->getAnyNominal())) {
          SemanticContext = SemanticContextKind::CurrentNominal;
          if (Result.FuncD && Result.FuncD->getDeclContext()->getSelfNominalTypeDecl() != BaseNominal)
            SemanticContext = SemanticContextKind::Super;
        } else if (BaseTy->is<TupleType>()) {
          SemanticContext = SemanticContextKind::CurrentNominal;
        }
      }
      if (Result.isSubscript()) {
        assert(SemanticContext != SemanticContextKind::None && BaseTy);
        auto *SD = dyn_cast_or_null<SubscriptDecl>(Result.FuncD);
        Lookup.addSubscriptCallPattern(Result.FuncTy->getAs<AnyFunctionType>(),
                                       SD, SemanticContext);
      } else {
        auto *FD = dyn_cast_or_null<AbstractFunctionDecl>(Result.FuncD);
        Lookup.addFunctionCallPattern(Result.FuncTy->getAs<AnyFunctionType>(),
                                      FD, SemanticContext);
      }
    }
    Lookup.setHaveLParen(false);

    shouldPerformGlobalCompletion |=
        !Lookup.FoundFunctionCalls || Lookup.FoundFunctionsWithoutFirstKeyword;
  } else if (!Results.empty()) {
    SmallVector<PossibleParamInfo, 8> Params;
    for (auto &Ret: Results) {
      shouldPerformGlobalCompletion |=
          addPossibleParams(Ret, Params, ExpectedTypes);
    }
    Lookup.addCallArgumentCompletionResults(Params);
  }

  if (shouldPerformGlobalCompletion) {
    for (auto &Result: Results) {
      ExpectedTypes.push_back(Result.ExpectedType);
    }
    Lookup.setExpectedTypes(ExpectedTypes, false);
    Lookup.getValueCompletionsInDeclContext(Loc);
    Lookup.getSelfTypeCompletionInDeclContext(Loc, /*isForDeclResult=*/false);

    // Add any keywords that can be used in an argument expr position.
    addSuperKeyword(CompletionCtx.getResultSink(), DC);
    addExprKeywords(CompletionCtx.getResultSink(), DC);
  }

  deliverCompletionResults(CompletionCtx, Lookup, DC, Consumer);
}

static void deliverUnresolvedMemberResults(
    ArrayRef<UnresolvedMemberTypeCheckCompletionCallback::Result> Results,
    DeclContext *DC, SourceLoc DotLoc,
    ide::CodeCompletionContext &CompletionCtx,
    CodeCompletionConsumer &Consumer) {
  ASTContext &Ctx = DC->getASTContext();
  CompletionLookup Lookup(CompletionCtx.getResultSink(), Ctx, DC,
                          &CompletionCtx);

  assert(DotLoc.isValid());
  Lookup.setHaveDot(DotLoc);
  Lookup.shouldCheckForDuplicates(Results.size() > 1);

  // Get the canonical versions of the top-level types
  SmallPtrSet<CanType, 4> originalTypes;
  for (auto &Result: Results)
    originalTypes.insert(Result.ExpectedTy->getCanonicalType());

  for (auto &Result: Results) {
    Lookup.setExpectedTypes({Result.ExpectedTy},
                            Result.IsImplicitSingleExpressionReturn,
                            /*expectsNonVoid*/true);
    Lookup.setIdealExpectedType(Result.ExpectedTy);

    // For optional types, also get members of the unwrapped type if it's not
    // already equivalent to one of the top-level types. Handling it via the top
    // level type and not here ensures we give the correct type relation
    // (identical, rather than convertible).
    if (Result.ExpectedTy->getOptionalObjectType()) {
      Type Unwrapped = Result.ExpectedTy->lookThroughAllOptionalTypes();
      if (originalTypes.insert(Unwrapped->getCanonicalType()).second)
        Lookup.getUnresolvedMemberCompletions(Unwrapped);
    }
    Lookup.getUnresolvedMemberCompletions(Result.ExpectedTy);
  }

  deliverCompletionResults(CompletionCtx, Lookup, DC, Consumer);
}

static void deliverKeyPathResults(
    ArrayRef<KeyPathTypeCheckCompletionCallback::Result> Results,
    DeclContext *DC, SourceLoc DotLoc,
    ide::CodeCompletionContext &CompletionCtx,
    CodeCompletionConsumer &Consumer) {
  ASTContext &Ctx = DC->getASTContext();
  CompletionLookup Lookup(CompletionCtx.getResultSink(), Ctx, DC,
                          &CompletionCtx);

  if (DotLoc.isValid()) {
    Lookup.setHaveDot(DotLoc);
  }
  Lookup.shouldCheckForDuplicates(Results.size() > 1);

  for (auto &Result : Results) {
    Lookup.setIsSwiftKeyPathExpr(Result.OnRoot);
    Lookup.getValueExprCompletions(Result.BaseType);
  }

  deliverCompletionResults(CompletionCtx, Lookup, DC, Consumer);
}

static void deliverDotExprResults(
    ArrayRef<DotExprTypeCheckCompletionCallback::Result> Results,
    Expr *BaseExpr, DeclContext *DC, SourceLoc DotLoc, bool IsInSelector,
    ide::CodeCompletionContext &CompletionCtx,
    CodeCompletionConsumer &Consumer) {
  ASTContext &Ctx = DC->getASTContext();
  CompletionLookup Lookup(CompletionCtx.getResultSink(), Ctx, DC,
                          &CompletionCtx);

  if (DotLoc.isValid())
    Lookup.setHaveDot(DotLoc);

  Lookup.setIsSuperRefExpr(isa<SuperRefExpr>(BaseExpr));

  if (auto *DRE = dyn_cast<DeclRefExpr>(BaseExpr))
    Lookup.setIsSelfRefExpr(DRE->getDecl()->getName() == Ctx.Id_self);

  if (isa<BindOptionalExpr>(BaseExpr) || isa<ForceValueExpr>(BaseExpr))
    Lookup.setIsUnwrappedOptional(true);

  if (IsInSelector) {
    Lookup.includeInstanceMembers();
    Lookup.setPreferFunctionReferencesToCalls();
  }

  Lookup.shouldCheckForDuplicates(Results.size() > 1);
  for (auto &Result: Results) {
    Lookup.setIsStaticMetatype(Result.BaseIsStaticMetaType);
    Lookup.getPostfixKeywordCompletions(Result.BaseTy, BaseExpr);
    Lookup.setExpectedTypes(Result.ExpectedTypes,
                            Result.IsImplicitSingleExpressionReturn,
                            Result.ExpectsNonVoid);
    if (isDynamicLookup(Result.BaseTy))
      Lookup.setIsDynamicLookup();
    Lookup.getValueExprCompletions(Result.BaseTy, Result.BaseDecl);
  }

  deliverCompletionResults(CompletionCtx, Lookup, DC, Consumer);
}

bool CodeCompletionCallbacksImpl::trySolverCompletion(bool MaybeFuncBody) {
  assert(ParsedExpr || CurDeclContext);

  SourceLoc CompletionLoc = ParsedExpr
    ? ParsedExpr->getLoc()
    : CurDeclContext->getASTContext().SourceMgr.getCodeCompletionLoc();

  switch (Kind) {
  case CompletionKind::DotExpr: {
    assert(CodeCompleteTokenExpr);
    assert(CurDeclContext);

    DotExprTypeCheckCompletionCallback Lookup(CurDeclContext,
                                              CodeCompleteTokenExpr);
    llvm::SaveAndRestore<TypeCheckCompletionCallback*>
      CompletionCollector(Context.CompletionCallback, &Lookup);
    typeCheckContextAt(CurDeclContext, CompletionLoc);

    // This (hopefully) only happens in cases where the expression isn't
    // typechecked during normal compilation either (e.g. member completion in a
    // switch case where there control expression is invalid). Having normal
    // typechecking still resolve even these cases would be beneficial for
    // tooling in general though.
    if (!Lookup.gotCallback())
      Lookup.fallbackTypeCheck();

    addKeywords(CompletionContext.getResultSink(), MaybeFuncBody);

    Expr *CheckedBase = CodeCompleteTokenExpr->getBase();
    deliverDotExprResults(Lookup.getResults(), CheckedBase, CurDeclContext,
                          DotLoc, isInsideObjCSelector(), CompletionContext,
                          Consumer);
    return true;
  }
  case CompletionKind::UnresolvedMember: {
    assert(CodeCompleteTokenExpr);
    assert(CurDeclContext);

    UnresolvedMemberTypeCheckCompletionCallback Lookup(CodeCompleteTokenExpr);
    llvm::SaveAndRestore<TypeCheckCompletionCallback*>
      CompletionCollector(Context.CompletionCallback, &Lookup);
    typeCheckContextAt(CurDeclContext, CompletionLoc);

    if (!Lookup.gotCallback())
      Lookup.fallbackTypeCheck(CurDeclContext);

    addKeywords(CompletionContext.getResultSink(), MaybeFuncBody);
    deliverUnresolvedMemberResults(Lookup.getResults(), CurDeclContext, DotLoc,
                                   CompletionContext, Consumer);
    return true;
  }
  case CompletionKind::KeyPathExprSwift: {
    assert(CurDeclContext);

    // CodeCompletionCallbacks::completeExprKeyPath takes a \c KeyPathExpr,
    // so we can safely cast the \c ParsedExpr back to a \c KeyPathExpr.
    auto KeyPath = cast<KeyPathExpr>(ParsedExpr);
    KeyPathTypeCheckCompletionCallback Lookup(KeyPath);
    llvm::SaveAndRestore<TypeCheckCompletionCallback *> CompletionCollector(
        Context.CompletionCallback, &Lookup);
    typeCheckContextAt(CurDeclContext, CompletionLoc);

    deliverKeyPathResults(Lookup.getResults(), CurDeclContext, DotLoc,
                          CompletionContext, Consumer);
    return true;
  }
  case CompletionKind::CallArg: {
    assert(CodeCompleteTokenExpr);
    assert(CurDeclContext);
    ArgumentTypeCheckCompletionCallback Lookup(CodeCompleteTokenExpr);
    llvm::SaveAndRestore<TypeCheckCompletionCallback*>
      CompletionCollector(Context.CompletionCallback, &Lookup);
    typeCheckContextAt(CurDeclContext, CompletionLoc);

    if (!Lookup.gotCallback())
      Lookup.fallbackTypeCheck(CurDeclContext);

    deliverArgumentResults(Lookup.getResults(), ShouldCompleteCallPatternAfterParen,
                           CompletionLoc, CurDeclContext, CompletionContext, Consumer);
    return true;
  }
  default:
    return false;
  }
}

// Undoes the single-expression closure/function body transformation on the
// given DeclContext and its parent contexts if they have a single expression
// body that contains the code completion location.
//
// FIXME: Remove this once all expression position completions are migrated
// to work via TypeCheckCompletionCallback.
static void undoSingleExpressionReturn(DeclContext *DC) {
  auto updateBody = [](BraceStmt *BS, ASTContext &Ctx) -> bool {
    ASTNode LastElem = BS->getLastElement();
    auto *RS = dyn_cast_or_null<ReturnStmt>(LastElem.dyn_cast<Stmt*>());

    if (!RS || !RS->isImplicit())
      return false;

    BS->setLastElement(RS->getResult());
    return true;
  };

  while (ClosureExpr *CE = dyn_cast_or_null<ClosureExpr>(DC)) {
    if (CE->hasSingleExpressionBody()) {
      if (updateBody(CE->getBody(), CE->getASTContext()))
        CE->setBody(CE->getBody(), false);
    }
    DC = DC->getParent();
  }
  if (FuncDecl *FD = dyn_cast_or_null<FuncDecl>(DC)) {
    if (FD->hasSingleExpressionBody()) {
      if (updateBody(FD->getBody(), FD->getASTContext()))
        FD->setHasSingleExpressionBody(false);
    }
  }
}

void CodeCompletionCallbacksImpl::doneParsing() {
  CompletionContext.CodeCompletionKind = Kind;

  if (Kind == CompletionKind::None) {
    return;
  }

  bool MaybeFuncBody = true;
  if (CurDeclContext) {
    auto *CD = CurDeclContext->getLocalContext();
    if (!CD || CD->getContextKind() == DeclContextKind::Initializer ||
        CD->getContextKind() == DeclContextKind::TopLevelCodeDecl)
      MaybeFuncBody = false;
  }

  if (auto *DC = dyn_cast_or_null<DeclContext>(ParsedDecl)) {
    if (DC->isChildContextOf(CurDeclContext))
      CurDeclContext = DC;
  }

  if (trySolverCompletion(MaybeFuncBody))
    return;

  undoSingleExpressionReturn(CurDeclContext);
  typeCheckContextAt(
      CurDeclContext,
      ParsedExpr
          ? ParsedExpr->getLoc()
          : CurDeclContext->getASTContext().SourceMgr.getCodeCompletionLoc());

  // Add keywords even if type checking fails completely.
  addKeywords(CompletionContext.getResultSink(), MaybeFuncBody);

  Optional<Type> ExprType;
  ConcreteDeclRef ReferencedDecl = nullptr;
  if (ParsedExpr) {
    if (auto *checkedExpr = findParsedExpr(CurDeclContext,
                                           ParsedExpr->getSourceRange())) {
      ParsedExpr = checkedExpr;
    }

    if (auto typechecked = typeCheckParsedExpr()) {
      ExprType = typechecked->first;
      ReferencedDecl = typechecked->second;
      ParsedExpr->setType(*ExprType);
    }

    if (!ExprType && Kind != CompletionKind::PostfixExprParen &&
        Kind != CompletionKind::CallArg &&
        Kind != CompletionKind::KeyPathExprObjC)
      return;
  }

  if (!ParsedTypeLoc.isNull() && !typecheckParsedType())
    return;

  CompletionLookup Lookup(CompletionContext.getResultSink(), P.Context,
                          CurDeclContext, &CompletionContext);
  if (ExprType) {
    Lookup.setIsStaticMetatype(ParsedExpr->isStaticallyDerivedMetatype());
  }
  if (auto *DRE = dyn_cast_or_null<DeclRefExpr>(ParsedExpr)) {
    Lookup.setIsSelfRefExpr(DRE->getDecl()->getName() == Context.Id_self);
  } else if (ParsedExpr && isa<SuperRefExpr>(ParsedExpr)) {
    Lookup.setIsSuperRefExpr();
  }

  if (isInsideObjCSelector())
    Lookup.includeInstanceMembers();
  if (PreferFunctionReferencesToCalls)
    Lookup.setPreferFunctionReferencesToCalls();

  auto DoPostfixExprBeginning = [&] (){
    SourceLoc Loc = P.Context.SourceMgr.getCodeCompletionLoc();
    Lookup.getValueCompletionsInDeclContext(Loc);
    Lookup.getSelfTypeCompletionInDeclContext(Loc, /*isForDeclResult=*/false);
  };

  switch (Kind) {
  case CompletionKind::None:
  case CompletionKind::DotExpr:
  case CompletionKind::UnresolvedMember:
  case CompletionKind::KeyPathExprSwift:
  case CompletionKind::CallArg:
    llvm_unreachable("should be already handled");
    return;

  case CompletionKind::StmtOrExpr:
  case CompletionKind::ForEachSequence:
  case CompletionKind::PostfixExprBeginning: {
    ExprContextInfo ContextInfo(CurDeclContext, CodeCompleteTokenExpr);
    Lookup.setExpectedTypes(ContextInfo.getPossibleTypes(),
                            ContextInfo.isImplicitSingleExpressionReturn());
    DoPostfixExprBeginning();
    break;
  }

  case CompletionKind::PostfixExpr: {
    Lookup.setHaveLeadingSpace(HasSpace);
    if (isDynamicLookup(*ExprType))
      Lookup.setIsDynamicLookup();
    Lookup.getValueExprCompletions(*ExprType, ReferencedDecl.getDecl());
    /// We set the type of ParsedExpr explicitly above. But we don't want an
    /// unresolved type in our AST when we type check again for operator
    /// completions. Remove the type of the ParsedExpr and see if we can come up
    /// with something more useful based on the the full sequence expression.
    if (ParsedExpr->getType()->is<UnresolvedType>()) {
      ParsedExpr->setType(nullptr);
    }
    Lookup.getOperatorCompletions(ParsedExpr, leadingSequenceExprs);
    Lookup.getPostfixKeywordCompletions(*ExprType, ParsedExpr);
    break;
  }

  case CompletionKind::PostfixExprParen: {
    Lookup.setHaveLParen(true);

    ExprContextInfo ContextInfo(CurDeclContext, CodeCompleteTokenExpr);

    if (ShouldCompleteCallPatternAfterParen) {
      ExprContextInfo ParentContextInfo(CurDeclContext, ParsedExpr);
      Lookup.setExpectedTypes(
          ParentContextInfo.getPossibleTypes(),
          ParentContextInfo.isImplicitSingleExpressionReturn());
      if (!ContextInfo.getPossibleCallees().empty()) {
        for (auto &typeAndDecl : ContextInfo.getPossibleCallees())
          Lookup.tryFunctionCallCompletions(typeAndDecl.Type, typeAndDecl.Decl,
                                            typeAndDecl.SemanticContext);
      } else if (ExprType && ((*ExprType)->is<AnyFunctionType>() ||
                              (*ExprType)->is<AnyMetatypeType>())) {
        Lookup.getValueExprCompletions(*ExprType, ReferencedDecl.getDecl());
      }
    } else {
      // Add argument labels, then fallthrough to get values.
      Lookup.addCallArgumentCompletionResults(ContextInfo.getPossibleParams());
    }

    if (!Lookup.FoundFunctionCalls ||
        (Lookup.FoundFunctionCalls &&
         Lookup.FoundFunctionsWithoutFirstKeyword)) {
      Lookup.setExpectedTypes(ContextInfo.getPossibleTypes(),
                              ContextInfo.isImplicitSingleExpressionReturn());
      Lookup.setHaveLParen(false);

      // Add any keywords that can be used in an argument expr position.
      addSuperKeyword(CompletionContext.getResultSink(), CurDeclContext);
      addExprKeywords(CompletionContext.getResultSink(), CurDeclContext);

      DoPostfixExprBeginning();
    }
    break;
  }

  case CompletionKind::KeyPathExprObjC: {
    if (DotLoc.isValid())
      Lookup.setHaveDot(DotLoc);
    Lookup.setIsKeyPathExpr();
    Lookup.includeInstanceMembers();

    if (ExprType) {
      if (isDynamicLookup(*ExprType))
        Lookup.setIsDynamicLookup();

      Lookup.getValueExprCompletions(*ExprType, ReferencedDecl.getDecl());
    } else {
      SourceLoc Loc = P.Context.SourceMgr.getCodeCompletionLoc();
      Lookup.getValueCompletionsInDeclContext(Loc, KeyPathFilter,
                                              /*LiteralCompletions=*/false);
    }
    break;
  }

  case CompletionKind::TypeDeclResultBeginning:
  case CompletionKind::TypeSimpleBeginning: {
    auto Loc = Context.SourceMgr.getCodeCompletionLoc();
    Lookup.getTypeCompletionsInDeclContext(Loc);
    Lookup.getSelfTypeCompletionInDeclContext(
        Loc, Kind == CompletionKind::TypeDeclResultBeginning);
    break;
  }

  case CompletionKind::TypeIdentifierWithDot: {
    Lookup.setHaveDot(SourceLoc());
    Lookup.getTypeCompletions(ParsedTypeLoc.getType());
    break;
  }

  case CompletionKind::TypeIdentifierWithoutDot: {
    Lookup.getTypeCompletions(ParsedTypeLoc.getType());
    break;
  }

  case CompletionKind::CaseStmtBeginning: {
    ExprContextInfo ContextInfo(CurDeclContext, CodeCompleteTokenExpr);
    Lookup.setExpectedTypes(ContextInfo.getPossibleTypes(),
                            ContextInfo.isImplicitSingleExpressionReturn());
    Lookup.setIdealExpectedType(CodeCompleteTokenExpr->getType());
    Lookup.getUnresolvedMemberCompletions(ContextInfo.getPossibleTypes());
    DoPostfixExprBeginning();
    break;
  }

  case CompletionKind::NominalMemberBeginning: {
    CompletionOverrideLookup OverrideLookup(CompletionContext.getResultSink(),
                                            P.Context, CurDeclContext,
                                            ParsedKeywords, introducerLoc);
    OverrideLookup.getOverrideCompletions(SourceLoc());
    break;
  }

  case CompletionKind::AccessorBeginning: {
    if (isa<AccessorDecl>(ParsedDecl)) {
      ExprContextInfo ContextInfo(CurDeclContext, CodeCompleteTokenExpr);
      Lookup.setExpectedTypes(ContextInfo.getPossibleTypes(),
                              ContextInfo.isImplicitSingleExpressionReturn());
      DoPostfixExprBeginning();
    }
    break;
  }

  case CompletionKind::AttributeBegin: {
    Lookup.getAttributeDeclCompletions(IsInSil, AttTargetDK);

    // TypeName at attribute position after '@'.
    // - VarDecl: Property Wrappers.
    // - ParamDecl/VarDecl/FuncDecl: Function Builders.
    if (!AttTargetDK || *AttTargetDK == DeclKind::Var ||
        *AttTargetDK == DeclKind::Param || *AttTargetDK == DeclKind::Func)
      Lookup.getTypeCompletionsInDeclContext(
          P.Context.SourceMgr.getCodeCompletionLoc());
    break;
  }
  case CompletionKind::AttributeDeclParen: {
    Lookup.getAttributeDeclParamCompletions(AttrKind, AttrParamIndex);
    break;
  }
  case CompletionKind::PoundAvailablePlatform: {
    Lookup.getPoundAvailablePlatformCompletions();
    break;
  }
  case CompletionKind::Import: {
    if (DotLoc.isValid())
      Lookup.addSubModuleNames(SubModuleNameVisibilityPairs);
    else
      Lookup.addImportModuleNames();
    break;
  }
  case CompletionKind::LabeledTrailingClosure: {
    ExprContextInfo ContextInfo(CurDeclContext, CodeCompleteTokenExpr);

    SmallVector<PossibleParamInfo, 2> params;
    // Only complete function type parameters
    llvm::copy_if(ContextInfo.getPossibleParams(), std::back_inserter(params),
                  [](const PossibleParamInfo &P) {
                    // nullptr indicates out of bounds.
                    if (!P.Param)
                      return true;
                    return P.Param->getPlainType()
                        ->lookThroughAllOptionalTypes()
                        ->is<AnyFunctionType>();
                  });

    bool allRequired = false;
    if (!params.empty()) {
      Lookup.addCallArgumentCompletionResults(
          params, /*isLabeledTrailingClosure=*/true);
      allRequired = llvm::all_of(
          params, [](const PossibleParamInfo &P) { return P.IsRequired; });
    }

    // If there're optional parameters, do global completion or member
    // completion depending on the completion is happening at the start of line.
    if (!allRequired) {
      if (IsAtStartOfLine) {
        //   foo() {}
        //   <HERE>

        auto &Sink = CompletionContext.getResultSink();
        if (isa<Initializer>(CurDeclContext))
          CurDeclContext = CurDeclContext->getParent();

        if (CurDeclContext->isTypeContext()) {
          // Override completion (CompletionKind::NominalMemberBeginning).
          addDeclKeywords(Sink, CurDeclContext,
                          Context.LangOpts.EnableExperimentalConcurrency,
                          Context.LangOpts.EnableExperimentalDistributed);
          addLetVarKeywords(Sink);
          SmallVector<StringRef, 0> ParsedKeywords;
          CompletionOverrideLookup OverrideLookup(Sink, Context, CurDeclContext,
                                                  ParsedKeywords, SourceLoc());
          OverrideLookup.getOverrideCompletions(SourceLoc());
        } else {
          // Global completion (CompletionKind::PostfixExprBeginning).
          addDeclKeywords(Sink, CurDeclContext,
                          Context.LangOpts.EnableExperimentalConcurrency,
                          Context.LangOpts.EnableExperimentalDistributed);
          addStmtKeywords(Sink, CurDeclContext, MaybeFuncBody);
          addSuperKeyword(Sink, CurDeclContext);
          addLetVarKeywords(Sink);
          addExprKeywords(Sink, CurDeclContext);
          addAnyTypeKeyword(Sink, Context.TheAnyType);
          DoPostfixExprBeginning();
        }
      } else {
        //   foo() {} <HERE>
        // Member completion.
        Expr *analyzedExpr = ContextInfo.getAnalyzedExpr();
        if (!analyzedExpr)
          break;

        // Only if the completion token is the last token in the call.
        if (analyzedExpr->getEndLoc() != CodeCompleteTokenExpr->getLoc())
          break;

        Type resultTy = analyzedExpr->getType();
        // If the call expression doesn't have a type, fallback to:
        if (!resultTy || resultTy->is<ErrorType>()) {
          // 1) Try to type check removing CodeCompletionExpr from the call.
          Expr *removedExpr = analyzedExpr;
          removeCodeCompletionExpr(CurDeclContext->getASTContext(),
                                   removedExpr);
          ConcreteDeclRef referencedDecl;
          auto optT = getTypeOfCompletionContextExpr(
              CurDeclContext->getASTContext(), CurDeclContext,
              CompletionTypeCheckKind::Normal, removedExpr, referencedDecl);
          if (optT) {
            resultTy = *optT;
            analyzedExpr->setType(resultTy);
          }
        }
        if (!resultTy || resultTy->is<ErrorType>()) {
          // 2) Infer it from the possible callee info.
          if (!ContextInfo.getPossibleCallees().empty()) {
            auto calleeInfo = ContextInfo.getPossibleCallees()[0];
            resultTy = calleeInfo.Type->getResult();
            analyzedExpr->setType(resultTy);
          }
        }
        if (!resultTy || resultTy->is<ErrorType>()) {
          // 3) Give up providing postfix completions.
          break;
        }

        auto &SM = CurDeclContext->getASTContext().SourceMgr;
        auto leadingChar =
            SM.extractText({SM.getCodeCompletionLoc().getAdvancedLoc(-1), 1});
        Lookup.setHaveLeadingSpace(leadingChar.find_first_of(" \t\f\v") !=
                                   StringRef::npos);

        if (isDynamicLookup(resultTy))
          Lookup.setIsDynamicLookup();
        Lookup.getValueExprCompletions(resultTy, /*VD=*/nullptr);
        Lookup.getOperatorCompletions(analyzedExpr, leadingSequenceExprs);
        Lookup.getPostfixKeywordCompletions(resultTy, analyzedExpr);
      }
    }
    break;
  }

  case CompletionKind::ReturnStmtExpr : {
    SourceLoc Loc = P.Context.SourceMgr.getCodeCompletionLoc();
    SmallVector<Type, 2> possibleReturnTypes;
    collectPossibleReturnTypesFromContext(CurDeclContext, possibleReturnTypes);
    Lookup.setExpectedTypes(possibleReturnTypes,
                            /*isImplicitSingleExpressionReturn*/ false);
    Lookup.getValueCompletionsInDeclContext(Loc);
    break;
  }

  case CompletionKind::YieldStmtExpr: {
    SourceLoc Loc = P.Context.SourceMgr.getCodeCompletionLoc();
    if (auto FD = dyn_cast<AccessorDecl>(CurDeclContext)) {
      if (FD->isCoroutine()) {
        // TODO: handle multi-value yields.
        Lookup.setExpectedTypes(FD->getStorage()->getValueInterfaceType(),
                                /*isImplicitSingleExpressionReturn*/ false);
      }
    }
    Lookup.getValueCompletionsInDeclContext(Loc);
    break;
  }

  case CompletionKind::AfterPoundExpr: {
    ExprContextInfo ContextInfo(CurDeclContext, CodeCompleteTokenExpr);
    Lookup.setExpectedTypes(ContextInfo.getPossibleTypes(),
                            ContextInfo.isImplicitSingleExpressionReturn());

    Lookup.addPoundAvailable(ParentStmtKind);
    Lookup.addPoundLiteralCompletions(/*needPound=*/false);
    Lookup.addObjCPoundKeywordCompletions(/*needPound=*/false);
    break;
  }

  case CompletionKind::AfterPoundDirective: {
    addPoundDirectives(CompletionContext.getResultSink());
    // FIXME: Add pound expressions (e.g. '#selector()') if it's at statements
    // position.
    break;
  }

  case CompletionKind::PlatformConditon: {
    addPlatformConditions(CompletionContext.getResultSink());
    addConditionalCompilationFlags(CurDeclContext->getASTContext(),
                                   CompletionContext.getResultSink());
    break;
  }

  case CompletionKind::GenericRequirement: {
    auto Loc = Context.SourceMgr.getCodeCompletionLoc();
    Lookup.getGenericRequirementCompletions(CurDeclContext, Loc);
    break;
  }
  case CompletionKind::PrecedenceGroup:
    Lookup.getPrecedenceGroupCompletions(SyntxKind);
    break;
  case CompletionKind::StmtLabel: {
    SourceLoc Loc = P.Context.SourceMgr.getCodeCompletionLoc();
    Lookup.getStmtLabelCompletions(Loc, ParentStmtKind == StmtKind::Continue);
    break;
  }
  case CompletionKind::TypeAttrBeginning: {
    Lookup.getTypeAttributeKeywordCompletions();

    // Type names at attribute position after '@'.
    Lookup.getTypeCompletionsInDeclContext(
      P.Context.SourceMgr.getCodeCompletionLoc());
    break;

  }
  case CompletionKind::AfterIfStmtElse:
  case CompletionKind::CaseStmtKeyword:
  case CompletionKind::EffectsSpecifier:
  case CompletionKind::ForEachPatternBeginning:
    // Handled earlier by keyword completions.
    break;
  }

  deliverCompletionResults(CompletionContext, Lookup, CurDeclContext, Consumer);
}

void PrintingCodeCompletionConsumer::handleResults(
    MutableArrayRef<CodeCompletionResult *> Results) {
  unsigned NumResults = 0;
  for (auto Result : Results) {
    if (!IncludeKeywords && Result->getKind() == CodeCompletionResult::Keyword)
      continue;
    ++NumResults;
  }
  if (NumResults == 0)
    return;

  OS << "Begin completions, " << NumResults << " items\n";
  for (auto Result : Results) {
    if (!IncludeKeywords && Result->getKind() == CodeCompletionResult::Keyword)
      continue;
    Result->printPrefix(OS);
    if (PrintAnnotatedDescription) {
      printCodeCompletionResultDescriptionAnnotated(*Result, OS, /*leadingPunctuation=*/false);
      OS << "; typename=";
      printCodeCompletionResultTypeNameAnnotated(*Result, OS);
    } else {
      Result->getCompletionString()->print(OS);
    }

    llvm::SmallString<64> Name;
    llvm::raw_svector_ostream NameOs(Name);
    Result->getCompletionString()->getName(NameOs);
    OS << "; name=" << Name;

    StringRef comment = Result->getBriefDocComment();
    if (IncludeComments && !comment.empty()) {
      OS << "; comment=" << comment;
    }

    OS << "\n";
  }
  OS << "End completions\n";
}

namespace {
class CodeCompletionCallbacksFactoryImpl
    : public CodeCompletionCallbacksFactory {
  CodeCompletionContext &CompletionContext;
  CodeCompletionConsumer &Consumer;

public:
  CodeCompletionCallbacksFactoryImpl(CodeCompletionContext &CompletionContext,
                                     CodeCompletionConsumer &Consumer)
      : CompletionContext(CompletionContext), Consumer(Consumer) {}

  CodeCompletionCallbacks *createCodeCompletionCallbacks(Parser &P) override {
    return new CodeCompletionCallbacksImpl(P, CompletionContext, Consumer);
  }
};
} // end anonymous namespace

CodeCompletionCallbacksFactory *
swift::ide::makeCodeCompletionCallbacksFactory(
    CodeCompletionContext &CompletionContext,
    CodeCompletionConsumer &Consumer) {
  return new CodeCompletionCallbacksFactoryImpl(CompletionContext, Consumer);
}

void swift::ide::lookupCodeCompletionResultsFromModule(
    CodeCompletionResultSink &targetSink, const ModuleDecl *module,
    ArrayRef<std::string> accessPath, bool needLeadingDot,
    const SourceFile *SF) {
  // Use the SourceFile as the decl context, to avoid decl context specific
  // behaviors.
  CompletionLookup Lookup(targetSink, module->getASTContext(), SF);
  Lookup.lookupExternalModuleDecls(module, accessPath, needLeadingDot);
}

MutableArrayRef<CodeCompletionResult *>
swift::ide::copyCodeCompletionResults(CodeCompletionResultSink &targetSink,
                                      CodeCompletionResultSink &sourceSink,
                                      bool onlyTypes,
                                      bool onlyPrecedenceGroups) {

  // We will be adding foreign results (from another sink) into TargetSink.
  // TargetSink should have an owning pointer to the allocator that keeps the
  // results alive.
  targetSink.ForeignAllocators.push_back(sourceSink.Allocator);
  auto startSize = targetSink.Results.size();

  if (onlyTypes) {
    std::copy_if(sourceSink.Results.begin(), sourceSink.Results.end(),
                 std::back_inserter(targetSink.Results),
                 [](CodeCompletionResult *R) -> bool {
      if (R->getKind() != CodeCompletionResult::Declaration)
        return false;
      switch(R->getAssociatedDeclKind()) {
      case CodeCompletionDeclKind::Module:
      case CodeCompletionDeclKind::Class:
      case CodeCompletionDeclKind::Struct:
      case CodeCompletionDeclKind::Enum:
      case CodeCompletionDeclKind::Protocol:
      case CodeCompletionDeclKind::TypeAlias:
      case CodeCompletionDeclKind::AssociatedType:
      case CodeCompletionDeclKind::GenericTypeParam:
        return true;
      case CodeCompletionDeclKind::PrecedenceGroup:
      case CodeCompletionDeclKind::EnumElement:
      case CodeCompletionDeclKind::Constructor:
      case CodeCompletionDeclKind::Destructor:
      case CodeCompletionDeclKind::Subscript:
      case CodeCompletionDeclKind::StaticMethod:
      case CodeCompletionDeclKind::InstanceMethod:
      case CodeCompletionDeclKind::PrefixOperatorFunction:
      case CodeCompletionDeclKind::PostfixOperatorFunction:
      case CodeCompletionDeclKind::InfixOperatorFunction:
      case CodeCompletionDeclKind::FreeFunction:
      case CodeCompletionDeclKind::StaticVar:
      case CodeCompletionDeclKind::InstanceVar:
      case CodeCompletionDeclKind::LocalVar:
      case CodeCompletionDeclKind::GlobalVar:
        return false;
      }

      llvm_unreachable("Unhandled CodeCompletionDeclKind in switch.");
    });
  } else if (onlyPrecedenceGroups) {
    std::copy_if(sourceSink.Results.begin(), sourceSink.Results.end(),
                 std::back_inserter(targetSink.Results),
                 [](CodeCompletionResult *R) -> bool {
      return R->getAssociatedDeclKind() ==
               CodeCompletionDeclKind::PrecedenceGroup;
    });
  } else {
    targetSink.Results.insert(targetSink.Results.end(),
                              sourceSink.Results.begin(),
                              sourceSink.Results.end());
  }

  return llvm::makeMutableArrayRef(targetSink.Results.data() + startSize,
                                   targetSink.Results.size() - startSize);
}

void SimpleCachingCodeCompletionConsumer::handleResultsAndModules(
    CodeCompletionContext &context,
    ArrayRef<RequestedCachedModule> requestedModules,
    DeclContext *DC) {

  // Use the current SourceFile as the DeclContext so that we can use it to
  // perform qualified lookup, and to get the correct visibility for
  // @testable imports. Also it cannot use 'DC' since it would apply decl
  // context changes to cached results.
  const SourceFile *SF = DC->getParentSourceFile();

  for (auto &R : requestedModules) {
    // FIXME(thread-safety): lock the whole AST context.  We might load a
    // module.
    llvm::Optional<CodeCompletionCache::ValueRefCntPtr> V =
        context.Cache.get(R.Key);
    if (!V.hasValue()) {
      // No cached results found. Fill the cache.
      V = context.Cache.createValue();
      (*V)->Sink.annotateResult = context.getAnnotateResult();
      lookupCodeCompletionResultsFromModule(
          (*V)->Sink, R.TheModule, R.Key.AccessPath,
          R.Key.ResultsHaveLeadingDot, SF);
      context.Cache.set(R.Key, *V);
    }
    assert(V.hasValue());
    auto newItems =
        copyCodeCompletionResults(context.getResultSink(), (*V)->Sink,
                                  R.OnlyTypes, R.OnlyPrecedenceGroups);
    postProcessResults(newItems, context.CodeCompletionKind, DC,
                       &context.getResultSink());
  }

  handleResults(context.takeResults());
}

//===----------------------------------------------------------------------===//
// ImportDepth
//===----------------------------------------------------------------------===//

ImportDepth::ImportDepth(ASTContext &context,
                         const FrontendOptions &frontendOptions) {
  llvm::DenseSet<ModuleDecl *> seen;
  std::deque<std::pair<ModuleDecl *, uint8_t>> worklist;

  StringRef mainModule = frontendOptions.ModuleName;
  auto *main = context.getLoadedModule(context.getIdentifier(mainModule));
  assert(main && "missing main module");
  worklist.emplace_back(main, uint8_t(0));

  // Imports from -import-name such as Playground auxiliary sources are treated
  // specially by applying import depth 0.
  llvm::StringSet<> auxImports;
  for (const auto &pair : frontendOptions.getImplicitImportModuleNames())
    auxImports.insert(pair.first);

  // Private imports from this module.
  // FIXME: only the private imports from the current source file.
  // FIXME: ImportFilterKind::ShadowedByCrossImportOverlay?
  SmallVector<ImportedModule, 16> mainImports;
  main->getImportedModules(mainImports,
                           {ModuleDecl::ImportFilterKind::Default,
                            ModuleDecl::ImportFilterKind::ImplementationOnly});
  for (auto &import : mainImports) {
    uint8_t depth = 1;
    if (auxImports.count(import.importedModule->getName().str()))
      depth = 0;
    worklist.emplace_back(import.importedModule, depth);
  }

  // Fill depths with BFS over module imports.
  while (!worklist.empty()) {
    ModuleDecl *module;
    uint8_t depth;
    std::tie(module, depth) = worklist.front();
    worklist.pop_front();

    if (!seen.insert(module).second)
      continue;

    // Insert new module:depth mapping.
    const clang::Module *CM = module->findUnderlyingClangModule();
    if (CM) {
      depths[CM->getFullModuleName()] = depth;
    } else {
      depths[module->getName().str()] = depth;
    }

    // Add imports to the worklist.
    SmallVector<ImportedModule, 16> imports;
    module->getImportedModules(imports);
    for (auto &import : imports) {
      uint8_t next = std::max(depth, uint8_t(depth + 1)); // unsigned wrap

      // Implicitly imported sub-modules get the same depth as their parent.
      if (const clang::Module *CMI =
              import.importedModule->findUnderlyingClangModule())
        if (CM && CMI->isSubModuleOf(CM))
          next = depth;
      worklist.emplace_back(import.importedModule, next);
    }
  }
}
