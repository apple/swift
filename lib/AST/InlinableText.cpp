//===---- InlinableText.cpp - Extract inlinable source text -----*- C++ -*-===//
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
#include "InlinableText.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTNode.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Expr.h"
#include "swift/Basic/Assertions.h"
#include "swift/Parse/Lexer.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/SmallString.h"

using namespace swift;

/// Gets the last token that exists inside this IfConfigClause, ignoring
/// hoisted elements.
///
/// If the clause is the last element, this returns the beginning of the line
/// before the parent IfConfigDecl's #endif token. Otherwise, it's the beginning
/// of the line before the next clause's #else or #elseif token.
static SourceLoc
getEffectiveEndLoc(SourceManager &sourceMgr, const IfConfigClause *clause,
                   const IfConfigDecl *decl) {
  auto clauses = decl->getClauses();
  if (clause == &clauses.back())
    return Lexer::getLocForStartOfLine(sourceMgr, decl->getEndLoc());

  assert(clause >= clauses.begin() && clause < clauses.end() &&
         "clauses must be contiguous");

  auto *nextClause = clause + 1;
  return Lexer::getLocForStartOfLine(sourceMgr, nextClause->Loc);
}

namespace {

class IsFeatureCheck : public ASTWalker {
public:
  bool foundFeature = false;

  /// Walk everything that's available.
  MacroWalking getMacroWalkingBehavior() const override {
    return MacroWalking::ArgumentsAndExpansion;
  }

  PreWalkResult<Expr *> walkToExprPre(Expr *expr) override {
    if (auto unresolved = dyn_cast<UnresolvedDeclRefExpr>(expr)) {
      if (unresolved->getName().getBaseName().userFacingName().starts_with("$"))
        foundFeature = true;
    }

    if (auto call = dyn_cast<CallExpr>(expr)) {
      if (auto unresolved = dyn_cast<UnresolvedDeclRefExpr>(call->getFn())) {
        StringRef userFacing = unresolved->getName().getBaseName()
            .userFacingName();
        if (userFacing == "compiler" || userFacing == "_compiler_version")
          foundFeature = true;
      }
    }

    return Action::SkipNodeIf(foundFeature, expr);
  }
};

bool clauseIsFeatureCheck(Expr *cond) {
  IsFeatureCheck checker;
  cond->walk(checker);
  return checker.foundFeature;
}

/// Whether any of the clauses here involves a feature check
/// (e.g., $AsyncAwait).
bool anyClauseIsFeatureCheck(ArrayRef<IfConfigClause> clauses) {
  for (const auto &clause : clauses) {
    if (Expr *cond = clause.Cond) {
      if (clauseIsFeatureCheck(cond))
        return true;
    }
  }

  return false;
}

/// A walker that searches through #if declarations, finding all text that does
/// not contribute to the final evaluated AST.
///
/// For example, in the following code:
/// ```
/// #if true
/// print("true")
/// #else
/// print("false")
/// #endif
/// ```
/// ExtractInactiveRanges will return the ranges (with leading newlines) of:
/// ```
/// #if true
/// #else
/// print("false")
/// #endif
/// ```
/// Leaving behind just 'print("true")'s range.
struct ExtractInactiveRanges : public ASTWalker {
  SmallVector<CharSourceRange, 4> ranges;
  SourceManager &sourceMgr;

  /// Walk everything that's available.
  MacroWalking getMacroWalkingBehavior() const override {
    return MacroWalking::ArgumentsAndExpansion;
  }

  explicit ExtractInactiveRanges(SourceManager &sourceMgr)
    : sourceMgr(sourceMgr) {}

  /// Adds the two SourceLocs as a CharSourceRange to the set of ignored
  /// ranges.
  /// \note: This assumes each of these locs is a character location, not a
  ///        token location.
  void addRange(SourceLoc start, SourceLoc end) {
    auto charRange = CharSourceRange(sourceMgr, start, end);
    ranges.push_back(charRange);
  }

  PreWalkAction walkToDeclPre(Decl *d) override {
    auto icd = dyn_cast<IfConfigDecl>(d);
    if (!icd)
      return Action::Continue();

    auto start = Lexer::getLocForStartOfLine(sourceMgr, icd->getStartLoc());
    auto end = Lexer::getLocForEndOfLine(sourceMgr, icd->getEndLoc());

    auto clause = icd->getActiveClause();

    // If there's no active clause, add the entire #if...#endif block.
    if (!clause) {
      addRange(start, end);
      return Action::SkipNode();
    }

    // If the clause is checking for a particular feature with $ or a compiler
    // version, keep the whole thing.
    if (anyClauseIsFeatureCheck(icd->getClauses())) {
      return Action::SkipNode();
    }

    // Ignore range from beginning of '#if', '#elseif', or '#else' to the
    // beginning of the elements of this clause.
    auto elementsBegin = clause->Loc;
    // If there's a condition (e.g. this isn't a '#else' block), then ignore
    // everything up to the end of the condition.
    if (auto cond = clause->Cond) {
      elementsBegin = cond->getEndLoc();
    }
    addRange(start, Lexer::getLocForEndOfLine(sourceMgr, elementsBegin));

    // Ignore range from effective end of the elements of this clause to the
    // end of the '#endif'
    addRange(getEffectiveEndLoc(sourceMgr, clause, icd), end);

    // Walk into direct children of this node that are IfConfigDecls, because
    // the standard walker won't walk into them.
    for (auto &elt : clause->Elements)
      if (elt.isDecl(DeclKind::IfConfig))
        elt.get<Decl *>()->walk(*this);

    return Action::SkipNode();
  }

  /// Gets the ignored ranges in source order.
  ArrayRef<CharSourceRange> getSortedRanges() {
    std::sort(ranges.begin(), ranges.end(),
              [&](CharSourceRange r1, CharSourceRange r2) {
                assert(!r1.overlaps(r2) && "no overlapping ranges");
                return sourceMgr.isBeforeInBuffer(r1.getStart(), r2.getStart());
              });
    return ranges;
  }
};
} // end anonymous namespace

/// Appends the textual contents of the provided source range, stripping
/// the contents of comments that appear in the source.
///
/// Given that comments are treated as whitespace, this also appends a
/// space or newline (depending if the comment was multi-line and itself
/// had newlines in the body) in place of the comment, to avoid fusing tokens
/// together.
static void appendRange(
  SourceManager &sourceMgr, SourceLoc start, SourceLoc end,
  SmallVectorImpl<char> &scratch) {
  unsigned bufferID = sourceMgr.findBufferContainingLoc(start);
  unsigned offset = sourceMgr.getLocOffsetInBuffer(start, bufferID);
  unsigned endOffset = sourceMgr.getLocOffsetInBuffer(end, bufferID);

  // Strip comments from the chunk before adding it by re-lexing the range.
  LangOptions FakeLangOpts;
  Lexer lexer(FakeLangOpts, sourceMgr, bufferID, nullptr, LexerMode::Swift,
    HashbangMode::Disallowed, CommentRetentionMode::ReturnAsTokens,
    offset, endOffset);

  SourceLoc nonCommentStart = start;
  Token token;

  // Re-lex the range, and skip the full text of `tok::comment` tokens.
  while (!token.is(tok::eof)) {
    lexer.lex(token);

    // Skip over #sourceLocation's in the file.
    if (token.is(tok::pound_sourceLocation)) {

      // Append the text leading up to the #sourceLocation
      auto charRange = CharSourceRange(
        sourceMgr, nonCommentStart, token.getLoc());
      StringRef text = sourceMgr.extractText(charRange);
      scratch.append(text.begin(), text.end());

      // Skip to the right paren. We know the AST is already valid, so there's
      // definitely a right paren.
      while (!token.is(tok::r_paren)) {
        lexer.lex(token);
      }

      nonCommentStart = Lexer::getLocForEndOfToken(sourceMgr, token.getLoc());
    }

    if (token.is(tok::comment)) {
      // Grab the start of the full comment token (with leading trivia as well)
      SourceLoc commentLoc = token.getLoc();

      // Find the end of the token (with trailing trivia)
      SourceLoc endLoc = Lexer::getLocForEndOfToken(sourceMgr, token.getLoc());

      // The comment token's range includes leading/trailing whitespace, so trim
      // whitespace and only strip the portions of the comment that are not
      // whitespace.
      CharSourceRange range = CharSourceRange(sourceMgr, commentLoc, endLoc);
      StringRef fullTokenText = sourceMgr.extractText(range);
      unsigned leadingWhitespace = fullTokenText.size() - 
        fullTokenText.ltrim().size();
      if (leadingWhitespace > 0) {
        commentLoc = commentLoc.getAdvancedLoc(leadingWhitespace);
      }

      unsigned trailingWhitespace = fullTokenText.size() -
        fullTokenText.rtrim().size();
      if (trailingWhitespace > 0) {
        endLoc = endLoc.getAdvancedLoc(-trailingWhitespace);
      }

      // First, extract the text up to the start of the comment, including the
      // whitespace.
      auto charRange = CharSourceRange(sourceMgr, nonCommentStart, commentLoc);
      StringRef text = sourceMgr.extractText(charRange);
      scratch.append(text.begin(), text.end());

      // Next, search through the comment text to see if it's a block comment
      // with a newline. If so we need to re-insert a newline to avoid fusing
      // multi-line tokens together.
      auto commentTextRange = CharSourceRange(sourceMgr, commentLoc, endLoc);
      StringRef commentText = sourceMgr.extractText(commentTextRange);
      bool hasNewline = commentText.find_first_of("\n\r") != StringRef::npos;

      // Use a newline as a filler character if the comment itself had a newline
      // in it.
      char filler = hasNewline ? '\n' : ' ';

      // Append a single whitespace filler character, to avoid fusing tokens.
      scratch.push_back(filler);

      // Start the next region after the contents of the comment.
      nonCommentStart = endLoc;
    }
  }

  if (nonCommentStart.isValid() && nonCommentStart != end) {
    auto charRange = CharSourceRange(sourceMgr, nonCommentStart, end);
    StringRef text = sourceMgr.extractText(charRange);
    scratch.append(text.begin(), text.end());
  }
}

StringRef swift::extractInlinableText(SourceManager &sourceMgr, ASTNode node,
                                      SmallVectorImpl<char> &scratch) {
  // Extract inactive ranges from the text of the node.
  ExtractInactiveRanges extractor(sourceMgr);
  node.walk(extractor);

  // Begin piecing together active code ranges.

  // Get the full start and end of the provided node, as character locations.
  SourceLoc start = node.getStartLoc();
  SourceLoc end = Lexer::getLocForEndOfToken(sourceMgr, node.getEndLoc());
  for (auto &range : extractor.getSortedRanges()) {
    // Add the text from the current 'start' to this ignored range's start.
    appendRange(sourceMgr, start, range.getStart(), scratch);

    // Set 'start' to the end of this range, effectively skipping it.
    start = range.getEnd();
  }

  // If there's leftover unignored text, add it.
  if (start != end) {
    appendRange(sourceMgr, start, end, scratch);
  }

  return { scratch.data(), scratch.size() };
}
