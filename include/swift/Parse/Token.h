//===--- Token.h - Token interface ------------------------------*- C++ -*-===//
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
//  This file defines the Token interface.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_TOKEN_H
#define SWIFT_TOKEN_H

#include "swift/Basic/SourceLoc.h"
#include "swift/Basic/LLVM.h"
#include "swift/Parse/Token.h"
#include "swift/Config.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSwitch.h"

namespace swift {

enum class tok : uint8_t {
#define TOKEN(X) X,
#include "swift/AST/TokenKinds.def"

  NUM_TOKENS
};

/// If a token kind has determined text, return the text; otherwise assert.
StringRef getTokenText(tok kind);

/// Token - This structure provides full information about a lexed token.
/// It is not intended to be space efficient, it is intended to return as much
/// information as possible about each returned token.  This is expected to be
/// compressed into a smaller form if memory footprint is important.
///
class Token {
  /// Kind - The actual flavor of token this is.
  ///
  tok Kind;

  /// Whether this token is the first token on the line.
  unsigned AtStartOfLine : 1;

  /// Whether this token is an escaped `identifier` token.
  unsigned EscapedIdentifier : 1;
  
  /// Modifiers for string literals
  unsigned MultilineString : 1;

  /// Length of custom delimiter of "raw" string literals
  unsigned CustomDelimiterLen : 8;

  // Padding bits == 32 - 11;

  /// The length of the comment that precedes the token.
  unsigned CommentLength;

  /// Text - The actual string covered by the token in the source buffer.
  StringRef Text;

  StringRef trimComment() const {
    assert(hasComment() && "Has no comment to trim.");
    StringRef Raw(Text.begin() - CommentLength, CommentLength);
    return Raw.trim();
  }

public:
  Token(tok Kind, StringRef Text, unsigned CommentLength = 0)
          : Kind(Kind), AtStartOfLine(false), EscapedIdentifier(false),
            MultilineString(false), CustomDelimiterLen(0),
            CommentLength(CommentLength), Text(Text) {}

  Token() : Token(tok::NUM_TOKENS, {}, 0) {}

  tok getKind() const { return Kind; }
  void setKind(tok K) { Kind = K; }
  void clearCommentLength() { CommentLength = 0; }
  
  /// is/isNot - Predicates to check if this token is a specific kind, as in
  /// "if (Tok.is(tok::l_brace)) {...}".
  bool is(tok K) const { return Kind == K; }
  bool isNot(tok K) const { return Kind != K; }

  // Predicates to check to see if the token is any of a list of tokens.

  bool isAny(tok K1) const {
    return is(K1);
  }
  template <typename ...T>
  bool isAny(tok K1, tok K2, T... K) const {
    if (is(K1))
      return true;
    return isAny(K2, K...);
  }

  // Predicates to check to see if the token is not the same as any of a list.
  template <typename ...T>
  bool isNot(tok K1, T... K) const { return !isAny(K1, K...); }

  bool isBinaryOperator() const {
    return Kind == tok::oper_binary_spaced || Kind == tok::oper_binary_unspaced;
  }
  
  bool isAnyOperator() const {
    return isBinaryOperator() || Kind == tok::oper_postfix ||
           Kind == tok::oper_prefix;
  }
  bool isNotAnyOperator() const {
    return !isAnyOperator();
  }

  bool isEllipsis() const {
    return isAnyOperator() && Text == "...";
  }
  bool isNotEllipsis() const {
    return !isEllipsis();
  }

  bool isTilde() const {
    return isAnyOperator() && Text == "~";
  }

  /// Determine whether this token occurred at the start of a line.
  bool isAtStartOfLine() const { return AtStartOfLine; }

  /// Set whether this token occurred at the start of a line.
  void setAtStartOfLine(bool value) { AtStartOfLine = value; }
  
  /// True if this token is an escaped identifier token.
  bool isEscapedIdentifier() const { return EscapedIdentifier; }
  /// Set whether this token is an escaped identifier token.
  void setEscapedIdentifier(bool value) {
    assert((!value || Kind == tok::identifier) &&
           "only identifiers can be escaped identifiers");
    EscapedIdentifier = value;
  }
  
  bool isContextualKeyword(StringRef ContextKW) const {
    return isAny(tok::identifier, tok::contextual_keyword) &&
           !isEscapedIdentifier() && Text == ContextKW;
  }
  
  /// Return true if this is a contextual keyword that could be the start of a
  /// decl.
  bool isContextualDeclKeyword() const {
    if (isNot(tok::identifier) || isEscapedIdentifier() || Text.empty())
      return false;

    return llvm::StringSwitch<bool>(Text)
#define CONTEXTUAL_CASE(KW) .Case(#KW, true)
#define CONTEXTUAL_DECL_ATTR(KW, ...) CONTEXTUAL_CASE(KW)
#define CONTEXTUAL_DECL_ATTR_ALIAS(KW, ...) CONTEXTUAL_CASE(KW)
#define CONTEXTUAL_SIMPLE_DECL_ATTR(KW, ...) CONTEXTUAL_CASE(KW)
#include "swift/AST/Attr.def"
#undef CONTEXTUAL_CASE
      .Case("macro", true)
      .Default(false);
  }

  bool isContextualPunctuator(StringRef ContextPunc) const {
    return isAnyOperator() && Text == ContextPunc;
  }

  /// Determine whether the token can be an argument label.
  ///
  /// This covers all identifiers and keywords except those keywords
  /// used
  bool canBeArgumentLabel() const {
    // Identifiers, escaped identifiers, and '_' can be argument labels.
    if (is(tok::identifier) || isEscapedIdentifier() || is(tok::kw__)) {
      return true;
    }

    // inout cannot be used as an argument label.
    if (is(tok::kw_inout))
      return false;

    // All other keywords can be argument labels.
    return isKeyword();
  }

  /// True if the token is an identifier or '_'.
  bool isIdentifierOrUnderscore() const {
    return isAny(tok::identifier, tok::kw__);
  }

  /// True if the token is an l_paren token that does not start a new line.
  bool isFollowingLParen() const {
    return !isAtStartOfLine() && Kind == tok::l_paren;
  }
  
  /// True if the token is an l_square token that does not start a new line.
  bool isFollowingLSquare() const {
    return !isAtStartOfLine() && Kind == tok::l_square;
  }

  /// True if the token is any keyword.
  bool isKeyword() const {
    switch (Kind) {
#define KEYWORD(X) case tok::kw_##X: return true;
#include "swift/AST/TokenKinds.def"
    default: return false;
    }
  }

  /// True if the token is any literal.
  bool isLiteral() const {
    switch(Kind) {
    case tok::integer_literal:
    case tok::floating_literal:
    case tok::string_literal:
      return true;
    default:
      return false;
    }
  }

  bool isPunctuation() const {
    switch (Kind) {
#define PUNCTUATOR(Name, Str) case tok::Name: return true;
#include "swift/AST/TokenKinds.def"
    default: return false;
    }
  }

  /// True if the string literal token is multiline.
  bool isMultilineString() const {
    return MultilineString;
  }
  /// Count of extending escaping '#'.
  unsigned getCustomDelimiterLen() const {
    return CustomDelimiterLen;
  }
  /// Set characteristics of string literal token.
  void setStringLiteral(bool IsMultilineString, unsigned CustomDelimiterLen) {
    assert(Kind == tok::string_literal);
    this->MultilineString = IsMultilineString;
    this->CustomDelimiterLen = CustomDelimiterLen;
  }
  
  /// getLoc - Return a source location identifier for the specified
  /// offset in the current file.
  SourceLoc getLoc() const {
    return SourceLoc(llvm::SMLoc::getFromPointer(Text.begin()));
  }

  unsigned getLength() const { return Text.size(); }

  CharSourceRange getRange() const {
    return CharSourceRange(getLoc(), getLength());
  }

  bool hasComment() const {
    return CommentLength != 0;
  }

  CharSourceRange getCommentRange() const {
    if (CommentLength == 0)
      return CharSourceRange(SourceLoc(llvm::SMLoc::getFromPointer(Text.begin())),
                             0);
    auto TrimedComment = trimComment();
    return CharSourceRange(
      SourceLoc(llvm::SMLoc::getFromPointer(TrimedComment.begin())),
      TrimedComment.size());
  }
  
  SourceLoc getCommentStart() const {
    if (CommentLength == 0) return SourceLoc();
    return SourceLoc(llvm::SMLoc::getFromPointer(trimComment().begin()));
  }

  StringRef getRawText() const {
    return Text;
  }

  StringRef getText() const {
    if (EscapedIdentifier) {
      // Strip off the backticks on either side.
      assert(Text.front() == '`' && Text.back() == '`');
      return Text.slice(1, Text.size() - 1);
    }
    return Text;
  }

  void setText(StringRef T) { Text = T; }

  /// Set the token to the specified kind and source range.
  void setToken(tok K, StringRef T, unsigned CommentLength = 0) {
    Kind = K;
    Text = T;
    this->CommentLength = CommentLength;
    EscapedIdentifier = false;
    this->MultilineString = false;
    this->CustomDelimiterLen = 0;
    assert(this->CustomDelimiterLen == CustomDelimiterLen &&
           "custom string delimiter length > 255");
  }
};
  
} // end namespace swift


namespace llvm {
  template <typename T> struct isPodLike;
  template <>
  struct isPodLike<swift::Token> { static const bool value = true; };
} // end namespace llvm

#endif
