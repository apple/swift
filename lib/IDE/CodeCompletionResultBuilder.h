//===--- CodeCompletionResultBuilder.h - Build completion results ---------===//
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

#ifndef SWIFT_LIB_IDE_CODE_COMPLETION_RESULT_BUILDER_H
#define SWIFT_LIB_IDE_CODE_COMPLETION_RESULT_BUILDER_H

#include "swift/AST/Types.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/StringExtras.h"
#include "swift/IDE/CodeCompletionResult.h"
#include "swift/IDE/CodeCompletionResultSink.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSwitch.h"

namespace clang {
class Module;
}

namespace swift {
class Decl;
class ModuleDecl;

namespace ide {

class CodeCompletionStringPrinter;

class CodeCompletionResultBuilder {
  friend CodeCompletionStringPrinter;
  
  CodeCompletionResultSink &Sink;
  CodeCompletionResultKind Kind;
  SemanticContextKind SemanticContext;
  CodeCompletionFlair Flair;
  unsigned NumBytesToErase = 0;
  const Decl *AssociatedDecl = nullptr;
  bool IsAsync = false;
  bool HasAsyncAlternative = false;
  llvm::Optional<CodeCompletionLiteralKind> LiteralKind;
  CodeCompletionKeywordKind KeywordKind = CodeCompletionKeywordKind::None;
  unsigned CurrentNestingLevel = 0;
  SmallVector<CodeCompletionString::Chunk, 4> Chunks;
  llvm::PointerUnion<const ModuleDecl *, const clang::Module *>
      CurrentModule;
  bool Cancelled = false;
  ContextFreeNotRecommendedReason ContextFreeNotRecReason =
      ContextFreeNotRecommendedReason::None;
  ContextualNotRecommendedReason ContextualNotRecReason =
      ContextualNotRecommendedReason::None;
  StringRef BriefDocComment;

  /// The result type that this completion item produces.
  CodeCompletionResultType ResultType = CodeCompletionResultType::unknown();

  /// The context in which this completion item is used. Used to compute the
  /// type relation to \c ResultType.
  const ExpectedTypeContext *TypeContext = nullptr;
  const DeclContext *DC = nullptr;
  bool CanCurrDeclContextHandleAsync = false;

  void addChunkWithText(CodeCompletionString::Chunk::ChunkKind Kind,
                        StringRef Text);

  void addChunkWithTextNoCopy(CodeCompletionString::Chunk::ChunkKind Kind,
                              StringRef Text) {
    Chunks.push_back(CodeCompletionString::Chunk::createWithText(
        Kind, CurrentNestingLevel, Text));
  }

  void addSimpleChunk(CodeCompletionString::Chunk::ChunkKind Kind) {
    Chunks.push_back(
        CodeCompletionString::Chunk::createSimple(Kind,
                                                  CurrentNestingLevel));
  }

  CodeCompletionString::Chunk &getLastChunk() {
    return Chunks.back();
  }

  CodeCompletionResult *takeResult();
  void finishResult();

public:
  CodeCompletionResultBuilder(CodeCompletionResultSink &Sink,
                              CodeCompletionResultKind Kind,
                              SemanticContextKind SemanticContext)
      : Sink(Sink), Kind(Kind), SemanticContext(SemanticContext) {}

  ~CodeCompletionResultBuilder() {
    finishResult();
  }

  void cancel() {
    Cancelled = true;
  }

  /// Annotated results are requested by the client.
  ///
  /// This affects the structure of the CodeCompletionString.
  bool shouldAnnotateResults() {
    return Sink.annotateResult;
  }

  void setNumBytesToErase(unsigned N) {
    NumBytesToErase = N;
  }

  void setAssociatedDecl(const Decl *D);

  void setIsAsync(bool IsAsync) { this->IsAsync = IsAsync; }
  void setHasAsyncAlternative(bool HasAsyncAlternative) {
    this->HasAsyncAlternative = HasAsyncAlternative;
  }

  void setLiteralKind(CodeCompletionLiteralKind kind) { LiteralKind = kind; }
  void setKeywordKind(CodeCompletionKeywordKind kind) { KeywordKind = kind; }
  void setContextFreeNotRecommended(ContextFreeNotRecommendedReason Reason) {
    ContextFreeNotRecReason = Reason;
  }
  void setContextualNotRecommended(ContextualNotRecommendedReason Reason) {
    ContextualNotRecReason = Reason;
  }

  void addFlair(CodeCompletionFlair Options) {
    Flair |= Options;
  }

  /// Indicate that the code completion item does not produce something with a
  /// sensible result type, like a keyword or a method override suggestion.
  void setResultTypeNotApplicable() {
    ResultType = CodeCompletionResultType::notApplicable();
  }

  /// Set the result type of this code completion item and the context that the
  /// item may be used in.
  /// This is not a single unique type because for code completion we consider
  /// e.g. \c Int as producing both an \c Int metatype and an \c Int instance
  /// type.
  void setResultTypes(ArrayRef<Type> ResultTypes) {
    this->ResultType = CodeCompletionResultType(ResultTypes);
  }

  /// Set context in which this code completion item occurs. Used to compute the
  /// item's type relation.
  void setTypeContext(const ExpectedTypeContext &TypeContext,
                      const DeclContext *DC) {
    this->TypeContext = &TypeContext;
    this->DC = DC;
  }

  void setCanCurrDeclContextHandleAsync(bool CanCurrDeclContextHandleAsync) {
    this->CanCurrDeclContextHandleAsync = CanCurrDeclContextHandleAsync;
  }

  void withNestedGroup(CodeCompletionString::Chunk::ChunkKind Kind,
                  llvm::function_ref<void()> body);

  void addAccessControlKeyword(AccessLevel Access) {
    switch (Access) {
    case AccessLevel::Private:
      addChunkWithTextNoCopy(
          CodeCompletionString::Chunk::ChunkKind::AccessControlKeyword,
          "private ");
      break;
    case AccessLevel::FilePrivate:
      addChunkWithTextNoCopy(
          CodeCompletionString::Chunk::ChunkKind::AccessControlKeyword,
          "fileprivate ");
      break;
    case AccessLevel::Internal:
      // 'internal' is the default, don't add it.
      break;
    case AccessLevel::Package:
      addChunkWithTextNoCopy(
          CodeCompletionString::Chunk::ChunkKind::AccessControlKeyword,
          "package ");
      break;
    case AccessLevel::Public:
      addChunkWithTextNoCopy(
          CodeCompletionString::Chunk::ChunkKind::AccessControlKeyword,
          "public ");
      break;
    case AccessLevel::Open:
      addChunkWithTextNoCopy(
          CodeCompletionString::Chunk::ChunkKind::AccessControlKeyword,
          "open ");
      break;
    }
  }

  void addRequiredKeyword() {
    addChunkWithTextNoCopy(
        CodeCompletionString::Chunk::ChunkKind::AccessControlKeyword,
        "required ");
  }

  void addOverrideKeyword() {
    addChunkWithTextNoCopy(
        CodeCompletionString::Chunk::ChunkKind::OverrideKeyword, "override ");
  }

  void addDeclIntroducer(StringRef Text) {
    addChunkWithText(CodeCompletionString::Chunk::ChunkKind::DeclIntroducer,
                     Text);
  }

  void addBaseName(StringRef Text) {
    addChunkWithText(CodeCompletionString::Chunk::ChunkKind::BaseName, Text);
  }

  void addKeyword(StringRef Text) {
    addChunkWithText(CodeCompletionString::Chunk::ChunkKind::Keyword, Text);
  }

  void addTextChunk(StringRef Text) {
    addChunkWithText(CodeCompletionString::Chunk::ChunkKind::Text, Text);
  }

  void addAnnotatedTextChunk(StringRef Text) {
    addTextChunk(Text);
    getLastChunk().setIsAnnotation();
  }

  void addAnnotatedThrows() {
    addThrows();
    getLastChunk().setIsAnnotation();
  }

  void addThrows() {
    addChunkWithTextNoCopy(
       CodeCompletionString::Chunk::ChunkKind::EffectsSpecifierKeyword,
       " throws");
  }

  void addAnnotatedAsync() {
    addAsync();
    getLastChunk().setIsAnnotation();
  }

  void addAsync() {
    addChunkWithTextNoCopy(
       CodeCompletionString::Chunk::ChunkKind::EffectsSpecifierKeyword,
       " async");
  }

  void addAnnotatedRethrows() {
    addRethrows();
    getLastChunk().setIsAnnotation();
  }

  void addRethrows() {
    addChunkWithTextNoCopy(
        CodeCompletionString::Chunk::ChunkKind::EffectsSpecifierKeyword,
        " rethrows");
  }

  void addAnnotatedLeftParen() {
    addLeftParen();
    getLastChunk().setIsAnnotation();
  }

  void addLeftParen() {
    addChunkWithTextNoCopy(
        CodeCompletionString::Chunk::ChunkKind::LeftParen, "(");
  }

  void addAnnotatedRightParen() {
    addRightParen();
    getLastChunk().setIsAnnotation();
  }

  void addRightParen() {
    addChunkWithTextNoCopy(
        CodeCompletionString::Chunk::ChunkKind::RightParen, ")");
  }

  void addAnnotatedLeftBracket() {
    addLeftBracket();
    getLastChunk().setIsAnnotation();
  }

  void addLeftBracket() {
    addChunkWithTextNoCopy(
        CodeCompletionString::Chunk::ChunkKind::LeftBracket, "[");
  }

  void addAnnotatedRightBracket() {
    addRightBracket();
    getLastChunk().setIsAnnotation();
  }

  void addRightBracket() {
    addChunkWithTextNoCopy(
        CodeCompletionString::Chunk::ChunkKind::RightBracket, "]");
  }

  void addLeftAngle() {
    addChunkWithTextNoCopy(
        CodeCompletionString::Chunk::ChunkKind::LeftAngle, "<");
  }

  void addRightAngle() {
    addChunkWithTextNoCopy(
        CodeCompletionString::Chunk::ChunkKind::RightAngle, ">");
  }

  void addLeadingDot() {
    addDot();
  }

  void addDot() {
    addChunkWithTextNoCopy(CodeCompletionString::Chunk::ChunkKind::Dot, ".");
  }

  void addEllipsis() {
    addChunkWithTextNoCopy(CodeCompletionString::Chunk::ChunkKind::Ellipsis,
                           "...");
  }

  void addComma() {
    addChunkWithTextNoCopy(
        CodeCompletionString::Chunk::ChunkKind::Comma, ", ");
  }

  void addExclamationMark() {
    addChunkWithTextNoCopy(
        CodeCompletionString::Chunk::ChunkKind::ExclamationMark, "!");
  }

  void addQuestionMark() {
    addChunkWithTextNoCopy(
        CodeCompletionString::Chunk::ChunkKind::QuestionMark, "?");
  }

  void addEqual() {
    addChunkWithTextNoCopy(CodeCompletionString::Chunk::ChunkKind::Equal, "=");
  }

  void addDeclAttrParamKeyword(StringRef Name, ArrayRef<StringRef> Parameters,
                               StringRef Annotation, bool NeedSpecify) {
    addChunkWithText(CodeCompletionString::Chunk::ChunkKind::
                     DeclAttrParamKeyword, Name);
    if (!Parameters.empty()) {
      addLeftParen();
      for (auto Parameter : Parameters) {
        addSimpleNamedParameter(Parameter);
      }
      addRightParen();
    }
    if (NeedSpecify)
      addChunkWithText(CodeCompletionString::Chunk::ChunkKind::
                       DeclAttrParamColon, ": ");
    if (!Annotation.empty())
      addTypeAnnotation(Annotation);
  }

  void addDeclAttrKeyword(StringRef Name, StringRef Annotation) {
    addChunkWithText(CodeCompletionString::Chunk::ChunkKind::
                     DeclAttrKeyword, Name);
    if (!Annotation.empty())
      addTypeAnnotation(Annotation);
  }

  void addAttributeKeyword(StringRef Name, StringRef Annotation) {
    addChunkWithText(CodeCompletionString::Chunk::ChunkKind::Attribute, Name);
    if (!Annotation.empty())
      addTypeAnnotation(Annotation);
  }

  StringRef escapeKeyword(StringRef Word, bool escapeAllKeywords,
                          llvm::SmallString<16> &EscapedKeyword) {
    EscapedKeyword.clear();
    bool shouldEscape = false;
    if (escapeAllKeywords) {
#define KEYWORD(kw) .Case(#kw, true)
      shouldEscape = llvm::StringSwitch<bool>(Word)
#include "swift/AST/TokenKinds.def"
        .Default(false);
    } else {
      shouldEscape = !canBeArgumentLabel(Word);
    }

    if (!shouldEscape)
      return Word;

    EscapedKeyword.append("`");
    EscapedKeyword.append(Word);
    EscapedKeyword.append("`");
    return EscapedKeyword;
  }

  void addCallParameterColon() {
    addChunkWithText(CodeCompletionString::Chunk::ChunkKind::
                     CallArgumentColon, ": ");
  }

  void addSimpleNamedParameter(StringRef name) {
    withNestedGroup(CodeCompletionString::Chunk::ChunkKind::CallArgumentBegin, [&] {
      // Use internal, since we don't want the name to be outside the
      // placeholder.
      addChunkWithText(
          CodeCompletionString::Chunk::ChunkKind::CallArgumentInternalName,
          name);
    });
  }

  void addSimpleTypedParameter(StringRef Annotation, bool IsVarArg = false) {
    withNestedGroup(CodeCompletionString::Chunk::ChunkKind::CallArgumentBegin, [&] {
      addChunkWithText(
          CodeCompletionString::Chunk::ChunkKind::CallArgumentType,
          Annotation);
      if (IsVarArg)
        addEllipsis();
    });
  }

  void addCallArgument(Identifier Name, Identifier LocalName, Type Ty,
                       Type ContextTy, bool IsVarArg, bool IsInOut, bool IsIUO,
                       bool IsAutoClosure, bool UseUnderscoreLabel,
                       bool IsLabeledTrailingClosure, bool HasDefault);

  void addCallArgument(Identifier Name, Type Ty, Type ContextTy = Type()) {
    addCallArgument(Name, Identifier(), Ty, ContextTy,
                    /*IsVarArg=*/false, /*IsInOut=*/false, /*IsIUO=*/false,
                    /*IsAutoClosure=*/false, /*UseUnderscoreLabel=*/false,
                    /*IsLabeledTrailingClosure=*/false, /*HasDefault=*/false);
  }

  void addGenericParameter(StringRef Name) {
    withNestedGroup(CodeCompletionString::Chunk::ChunkKind::GenericParameterBegin,
               [&] {
      addChunkWithText(
        CodeCompletionString::Chunk::ChunkKind::GenericParameterName, Name);
    });
  }

  void addDynamicLookupMethodCallTail() {
    addChunkWithTextNoCopy(
        CodeCompletionString::Chunk::ChunkKind::DynamicLookupMethodCallTail,
        "!");
    getLastChunk().setIsAnnotation();
  }

  void addOptionalMethodCallTail() {
    addChunkWithTextNoCopy(
        CodeCompletionString::Chunk::ChunkKind::OptionalMethodCallTail, "?");
  }

  void addTypeAnnotation(StringRef Type) {
    addChunkWithText(
        CodeCompletionString::Chunk::ChunkKind::TypeAnnotation, Type);
    getLastChunk().setIsAnnotation();
  }

  void addTypeAnnotation(Type T, PrintOptions PO, StringRef suffix = "");

  void addBraceStmtWithCursor(StringRef Description = "") {
    addChunkWithText(
        CodeCompletionString::Chunk::ChunkKind::BraceStmtWithCursor,
        Description);
  }

  void addWhitespace(StringRef space) {
    addChunkWithText(
        CodeCompletionString::Chunk::ChunkKind::Whitespace, space);
  }

  void addAnnotatedWhitespace(StringRef space) {
    addWhitespace(space);
    getLastChunk().setIsAnnotation();
  }

  void setBriefDocComment(StringRef comment) {
    BriefDocComment = comment;
  }
};

} // namespace ide
} // namespace swift

#endif // SWIFT_LIB_IDE_CODE_COMPLETION_RESULT_BUILDER_H
