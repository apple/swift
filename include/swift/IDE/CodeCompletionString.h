//===--- CodeCompletionString.h -------------------------------------------===//
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

#ifndef SWIFT_IDE_CODECOMPLETIONSTRING_H
#define SWIFT_IDE_CODECOMPLETIONSTRING_H

#include "swift/Basic/Debug.h"
#include "swift/Basic/LLVM.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/TrailingObjects.h"

namespace swift {
namespace ide {

class CodeCompletionResultBuilder;

namespace detail {
class CodeCompletionStringChunk {
  friend class swift::ide::CodeCompletionResultBuilder;

public:
  enum class ChunkKind {
    /// "open", "public", "package", "internal", "fileprivate", or "private".
    AccessControlKeyword,

    /// such as @"available".
    DeclAttrKeyword,

    /// such as "unavailable" etc. for @available.
    DeclAttrParamKeyword,

    /// The "override" keyword.
    OverrideKeyword,

    /// The "throws", "rethrows" and "async" keyword.
    EffectsSpecifierKeyword,

    /// The keyword part of a declaration before the name, like "func".
    DeclIntroducer,

    /// Other generic keyword.
    Keyword,

    /// Other generic attributes.
    Attribute,

    /// Normal text chunk.
    Text,

    /// Base name of the result.
    BaseName,

    /// The first chunk of an optional substring that continues until
    /// \c NestingLevel decreases.
    OptionalBegin,

    /// Punctuation.
    LeftParen,
    RightParen,
    LeftBracket,
    RightBracket,
    LeftAngle,
    RightAngle,
    Dot,
    Ellipsis,
    Comma,
    ExclamationMark,
    QuestionMark,
    Ampersand,
    Equal,
    Whitespace,

    /// The first chunk of a whole generic parameter clause.
    /// E.g '<T, C: Collection>'
    GenericParameterClauseBegin,

    /// The first chunk of a generic quirement clause.
    /// E.g. 'where T: Collection, T.Element == Int'
    GenericRequirementClauseBegin,

    /// The first chunk of a substring that describes the parameter for a
    /// generic type.
    GenericParameterBegin,
    /// Generic type parameter name.
    GenericParameterName,

    /// The first chunk of a substring that describes the argument for a
    /// function call.
    CallArgumentBegin,

    /// Function call argument label.
    CallArgumentName,

    /// Function parameter internal / local name for an call argument. If the
    /// parameter has no formal API name, it can still have a local name which
    /// can be useful for display purposes.
    ///
    /// This chunk should not be inserted into the editor buffer.
    CallArgumentInternalName,

    /// A colon between argument name and value.  Should be inserted in the
    /// editor buffer if the preceding CallArgumentName was inserted.
    CallArgumentColon,

    /// A colon between parameter name and value. Used in decl attribute.
    DeclAttrParamColon,

    /// Required argument type.
    CallArgumentType,

    /// Argument type tag for annotated results.
    CallArgumentTypeBegin,

    /// Added if the argument has a default value.
    CallArgumentDefaultBegin,

    /// System type name.
    TypeIdSystem,

    /// Non-system type name.
    TypeIdUser,

    /// Desugared closure argument type. This can be used to get the
    /// closure type if CallArgumentType is a TypeAliasType.
    CallArgumentClosureType,

    /// An expanded closure expression for the value of an argument, including
    /// the left and right braces and possible signature. The preferred
    /// position to put the cursor after the completion result is inserted
    /// into the editor buffer is between the braces.
    CallArgumentClosureExpr,

    /// A placeholder for \c ! or \c ? in a call to a method found by dynamic
    /// lookup.
    ///
    /// The default spelling is \c !, but clients may render it as \c ? if
    /// desired.
    DynamicLookupMethodCallTail,

    /// A placeholder for \c ! or \c ? in a call to an optional method.
    ///
    /// The default spelling is \c !, but clients may render it as \c ? if
    /// desired.
    OptionalMethodCallTail,

    /// The first chunk of a substring that describes the single parameter
    /// declaration for a parameter clause.
    ParameterDeclBegin,

    ParameterDeclExternalName,

    ParameterDeclLocalName,

    ParameterDeclColon,

    ParameterDeclTypeBegin,

    /// Default argument clause for parameter declarations.
    DefaultArgumentClauseBegin,

    /// First chunk for effect specifiers. i.e. 'async' and 'throws'.
    EffectsSpecifierClauseBegin,

    /// First chunk for result type clause i.e. ' -> ResultTy' or ': ResultTy'.
    DeclResultTypeClauseBegin,

    /// First chunk for attribute and modifier list i.e. 'override public'
    AttributeAndModifierListBegin,

    /// Specifies the type of the whole entity that is returned in this code
    /// completion result.  For example, for variable references it is the
    /// variable type, for function calls it is the return type.
    ///
    /// This chunk should not be inserted into the editor buffer.
    TypeAnnotation,

    /// Structured group version of 'TypeAnnotation'.
    /// This grouped chunks should not be inserted into the editor buffer.
    TypeAnnotationBegin,

    /// A brace statement -- left brace and right brace.  The preferred
    /// position to put the cursor after the completion result is inserted
    /// into the editor buffer is between the braces.
    ///
    /// The spelling is always "{}", but clients may choose to insert newline
    /// and indentation in between.
    BraceStmtWithCursor,
  };

  static bool chunkStartsNestedGroup(ChunkKind Kind) {
    return Kind == ChunkKind::CallArgumentBegin ||
           Kind == ChunkKind::GenericParameterBegin ||
           Kind == ChunkKind::OptionalBegin ||
           Kind == ChunkKind::CallArgumentTypeBegin ||
           Kind == ChunkKind::CallArgumentDefaultBegin ||
           Kind == ChunkKind::TypeAnnotationBegin ||
           Kind == ChunkKind::ParameterDeclBegin ||
           Kind == ChunkKind::ParameterDeclTypeBegin ||
           Kind == ChunkKind::DefaultArgumentClauseBegin ||
           Kind == ChunkKind::GenericParameterClauseBegin ||
           Kind == ChunkKind::EffectsSpecifierClauseBegin ||
           Kind == ChunkKind::DeclResultTypeClauseBegin ||
           Kind == ChunkKind::GenericRequirementClauseBegin ||
           Kind == ChunkKind::AttributeAndModifierListBegin;
  }

  static bool chunkHasText(ChunkKind Kind) {
    return Kind == ChunkKind::AccessControlKeyword ||
           Kind == ChunkKind::OverrideKeyword ||
           Kind == ChunkKind::EffectsSpecifierKeyword ||
           Kind == ChunkKind::DeclAttrKeyword ||
           Kind == ChunkKind::DeclIntroducer ||
           Kind == ChunkKind::Keyword ||
           Kind == ChunkKind::Attribute ||
           Kind == ChunkKind::BaseName ||
           Kind == ChunkKind::Text ||
           Kind == ChunkKind::LeftParen ||
           Kind == ChunkKind::RightParen ||
           Kind == ChunkKind::LeftBracket ||
           Kind == ChunkKind::RightBracket ||
           Kind == ChunkKind::LeftAngle ||
           Kind == ChunkKind::RightAngle ||
           Kind == ChunkKind::Dot ||
           Kind == ChunkKind::Ellipsis ||
           Kind == ChunkKind::Comma ||
           Kind == ChunkKind::ExclamationMark ||
           Kind == ChunkKind::QuestionMark ||
           Kind == ChunkKind::Ampersand ||
           Kind == ChunkKind::Equal ||
           Kind == ChunkKind::Whitespace ||
           Kind == ChunkKind::CallArgumentName ||
           Kind == ChunkKind::CallArgumentInternalName ||
           Kind == ChunkKind::CallArgumentColon ||
           Kind == ChunkKind::CallArgumentType ||
           Kind == ChunkKind::CallArgumentClosureType ||
           Kind == ChunkKind::CallArgumentClosureExpr ||
           Kind == ChunkKind::ParameterDeclExternalName ||
           Kind == ChunkKind::ParameterDeclLocalName ||
           Kind == ChunkKind::ParameterDeclColon ||
           Kind == ChunkKind::DeclAttrParamColon ||
           Kind == ChunkKind::DeclAttrParamKeyword ||
           Kind == ChunkKind::GenericParameterName ||
           Kind == ChunkKind::DynamicLookupMethodCallTail ||
           Kind == ChunkKind::OptionalMethodCallTail ||
           Kind == ChunkKind::TypeAnnotation ||
           Kind == ChunkKind::BraceStmtWithCursor ||
           Kind == ChunkKind::TypeIdSystem ||
           Kind == ChunkKind::TypeIdUser;
  }

private:
  unsigned Kind : 8;
  unsigned NestingLevel : 8;

  /// If true, then this chunk is an annotation that is included only
  /// for exposition and may not be inserted in the editor buffer.
  unsigned IsAnnotation : 1;

  StringRef Text;

  CodeCompletionStringChunk(ChunkKind Kind, unsigned NestingLevel,
                            StringRef Text, bool isAnnotation)
      : Kind(unsigned(Kind)), NestingLevel(NestingLevel),
        IsAnnotation(isAnnotation), Text(Text) {
    assert(chunkHasText(Kind));
  }

  CodeCompletionStringChunk(ChunkKind Kind, unsigned NestingLevel,
                            bool isAnnotation)
      : Kind(unsigned(Kind)), NestingLevel(NestingLevel),
        IsAnnotation(isAnnotation) {
    assert(!chunkHasText(Kind));
  }

  void setIsAnnotation() { IsAnnotation = 1; }

public:
  ChunkKind getKind() const { return ChunkKind(Kind); }

  bool is(ChunkKind K) const { return getKind() == K; }

  unsigned getNestingLevel() const { return NestingLevel; }

  bool isAnnotation() const { return IsAnnotation; }

  bool hasText() const { return chunkHasText(getKind()); }

  StringRef getText() const {
    assert(hasText());
    return Text;
  }

  bool endsPreviousNestedGroup(unsigned GroupNestingLevel) const {
    return NestingLevel < GroupNestingLevel ||
           (NestingLevel == GroupNestingLevel &&
            chunkStartsNestedGroup(getKind()));
  }

  static CodeCompletionStringChunk createWithText(ChunkKind Kind,
                                                  unsigned NestingLevel,
                                                  StringRef Text,
                                                  bool isAnnotation = false) {
    return CodeCompletionStringChunk(Kind, NestingLevel, Text, isAnnotation);
  }

  static CodeCompletionStringChunk createSimple(ChunkKind Kind,
                                                unsigned NestingLevel,
                                                bool isAnnotation = false) {
    return CodeCompletionStringChunk(Kind, NestingLevel, isAnnotation);
  }
};

} // end namespace detail

/// A structured representation of a code completion string.
class alignas(detail::CodeCompletionStringChunk) CodeCompletionString final
    : private llvm::TrailingObjects<CodeCompletionString,
                                    detail::CodeCompletionStringChunk> {
  friend class CodeCompletionResultBuilder;
  friend TrailingObjects;

public:
  using Chunk = detail::CodeCompletionStringChunk;

private:
  unsigned NumChunks : 16;

  CodeCompletionString(ArrayRef<Chunk> Chunks);

public:
  /// Creates a \c CodeCompletionString from a list of \c Chunks.
  ///
  /// \note The caller must ensure any text inside \c Chunks will outlive this
  /// object, typically by storing them inside a \c CodeCompletionResultSink.
  static CodeCompletionString *create(llvm::BumpPtrAllocator &Allocator,
                                      ArrayRef<Chunk> Chunks);

  ArrayRef<Chunk> getChunks() const {
    return {getTrailingObjects<Chunk>(), NumChunks};
  }

  StringRef getFirstTextChunk(bool includeLeadingPunctuation = false) const;
  llvm::Optional<unsigned>
  getFirstTextChunkIndex(bool includeLeadingPunctuation = false) const;

  /// Print a debug representation of the code completion string to \p OS.
  void print(raw_ostream &OS) const;
  SWIFT_DEBUG_DUMP;
};

} // end namespace ide
} // end namespace swift

#endif // SWIFT_IDE_CODECOMPLETIONSTRING_H
