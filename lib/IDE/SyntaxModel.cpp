//===--- SyntaxModel.cpp - Routines for IDE syntax model ------------------===//
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

#include "swift/IDE/SyntaxModel.h"
#include "swift/Basic/Defer.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Expr.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/Module.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/TypeRepr.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Parse/Lexer.h"
#include "swift/Parse/Token.h"
#include "swift/Config.h"
#include "swift/Subsystems.h"
#include "clang/Basic/CharInfo.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/MemoryBuffer.h"
#include <vector>
#include <regex>

using namespace swift;
using namespace ide;

void SyntaxModelWalker::anchor() {}

struct SyntaxModelContext::Implementation {
  SourceFile &SrcFile;
  const LangOptions &LangOpts;
  const SourceManager &SrcMgr;
  std::vector<SyntaxNode> TokenNodes;

  Implementation(SourceFile &SrcFile)
    : SrcFile(SrcFile),
      LangOpts(SrcFile.getASTContext().LangOpts),
      SrcMgr(SrcFile.getASTContext().SourceMgr) {}
};

/// Matches the tokens in the argument of an image or file literal expression if
/// its argument is itself a literal string, e.g:
///   #imageLiteral(resourceName: "foo.png")
///   #fileLiteral(resourceName: "foo.txt")
/// If the given tokens start with the expected tokens and they all appear on
///  the same line, the source location beyond the final matched token and
///  number of matched tokens are returned. Otherwise None is returned.
static llvm::Optional<Located<unsigned>>
matchImageOrFileLiteralArg(ArrayRef<Token> Tokens) {
  const unsigned NUM_TOKENS = 5;
  if (Tokens.size() < NUM_TOKENS)
    return llvm::None;
  const tok kinds[NUM_TOKENS] = {
      tok::l_paren,
      tok::identifier, tok::colon, tok::string_literal,
      tok::r_paren
  };
  for (unsigned i = 0; i < NUM_TOKENS; ++i) {
    // FIXME: some editors don't handle multi-line object literals very well,
    // so don't report them as object literals for now.
    if (Tokens[i].getKind() != kinds[i] || Tokens[i].isAtStartOfLine())
      return llvm::None;
  }
  if (Tokens[1].getText() != "resourceName")
    return llvm::None;
  auto EndToken = Tokens[NUM_TOKENS-1];
  return Located<unsigned>(NUM_TOKENS, EndToken.getLoc().getAdvancedLoc(EndToken.getLength()));
}

/// Matches the tokens in the argument of an image literal expression if its
/// arguments are themselves number literals, e.g:
///   #colorLiteral(red: 1.0, green: 1.0, blue: 0.5, alpha: 1.0)
/// If the given tokens start with the expected tokens and they all appear on
/// the same line, the source location beyond the final matched token and number
/// of matched tokens are returned. Otherwise None is returned.
static llvm::Optional<Located<unsigned>>
matchColorLiteralArg(ArrayRef<Token> Tokens) {
  const unsigned NUM_TOKENS = 17;
  if (Tokens.size() < NUM_TOKENS)
    return llvm::None;
  const tok kinds[NUM_TOKENS] = {
    tok::l_paren,
    tok::identifier, tok::colon, tok::floating_literal, tok::comma,
    tok::identifier, tok::colon, tok::floating_literal, tok::comma,
    tok::identifier, tok::colon, tok::floating_literal, tok::comma,
    tok::identifier, tok::colon, tok::floating_literal,
    tok::r_paren
  };
  for (unsigned i = 0; i < NUM_TOKENS; ++i) {
    auto Kind = Tokens[i].getKind();
    if (Kind == tok::integer_literal)
        Kind = tok::floating_literal;
    // FIXME: some editors don't handle multi-line object literals very well,
    // so don't report them as object literals for now.
    if (Kind != kinds[i] || Tokens[i].isAtStartOfLine())
      return llvm::None;
  }
  if (Tokens[1].getText() != "red" || Tokens[5].getText() != "green" ||
      Tokens[9].getText() != "blue" || Tokens[13].getText() != "alpha")
    return llvm::None;
  auto EndToken = Tokens[NUM_TOKENS-1];
  return Located<unsigned>(NUM_TOKENS, EndToken.getLoc().getAdvancedLoc(EndToken.getLength()));
}

SyntaxModelContext::SyntaxModelContext(SourceFile &SrcFile)
  : Impl(*new Implementation(SrcFile)) {
  const bool IsPlayground = Impl.LangOpts.Playground;
  const SourceManager &SM = Impl.SrcMgr;
  ArrayRef<Token> Tokens = SrcFile.getAllTokens();
  std::vector<SyntaxNode> Nodes;
  SourceLoc AttrLoc;
  SourceLoc UnaryMinusLoc;
  for (unsigned I = 0, E = Tokens.size(); I != E; ++I) {
    auto &Tok = Tokens[I];
    // Ignore empty string literals between interpolations, e.g. "\(1)\(2)"
    if (!Tok.getLength())
        continue;
    SyntaxNodeKind Kind;
    SourceLoc Loc;
    llvm::Optional<unsigned> Length;
    if (AttrLoc.isValid()) {
      // This token is following @, see if it's a known attribute name.
      // Type attribute, decl attribute, or '@unknown' for swift case statement.
      if (TypeAttributes::getAttrKindFromString(Tok.getText()) != TAK_Count ||
          DeclAttribute::getAttrKindFromString(Tok.getText()) != DAK_Count ||
          Tok.getText() == "unknown")  {
        // It's a known attribute, so treat it as a syntactic attribute node for
        // syntax coloring. If swift gets user attributes then all identifiers
        // will be treated as syntactic attribute nodes.
        Loc = AttrLoc;
        Length = SM.getByteDistance(Loc, Tok.getLoc()) + Tok.getLength();
        Kind = SyntaxNodeKind::AttributeId;
      }
      AttrLoc = SourceLoc();
    }

    if (!Loc.isValid()) {
      Loc = Tok.getLoc();
      Length = Tok.getLength();

      switch(Tok.getKind()) {
#define KEYWORD(X) case tok::kw_##X:
#include "swift/AST/TokenKinds.def"
#undef KEYWORD
      case tok::contextual_keyword:
        Kind = SyntaxNodeKind::Keyword;
        break;

      // Note: the below only handles object literals where each argument is a
      // single literal. If the arguments are more complex than that we rely on
      // there being an ObjectLiteralExpr in the AST and convert the individual
      // tokens within its range into a single object literal in
      // ModelASTWalker. We only bother with the below so that in the most
      // common cases we still present object literals as object literals when
      // the ObjectLiteralExpr doesn't appear in the AST (which can happen when
      // they appear within an invalid expression).
      case tok::pound_fileLiteral:
      case tok::pound_imageLiteral:
        if (auto Match = matchImageOrFileLiteralArg(Tokens.slice(I+1))) {
          Kind = SyntaxNodeKind::ObjectLiteral;
          Length = SM.getByteDistance(Loc, Match->Loc);
          // skip over the extra matched tokens
          I += Match->Item - 1;
        } else {
          Kind = SyntaxNodeKind::Keyword;
        }
        break;
      case tok::pound_colorLiteral:
        if (auto Match = matchColorLiteralArg(Tokens.slice(I+1))) {
          Kind = SyntaxNodeKind::ObjectLiteral;
          Length = SM.getByteDistance(Loc, Match->Loc);
          // skip over the matches tokens
          I += Match->Item - 1;
        } else {
          Kind = SyntaxNodeKind::Keyword;
        }
        break;

#define POUND_COND_DIRECTIVE_KEYWORD(Name) case tok::pound_##Name:
#include "swift/AST/TokenKinds.def"
        Kind = SyntaxNodeKind::BuildConfigKeyword;
        break;

#define POUND_DIRECTIVE_KEYWORD(Name) case tok::pound_##Name:
#define POUND_COND_DIRECTIVE_KEYWORD(Name)
#include "swift/AST/TokenKinds.def"
        Kind = SyntaxNodeKind::PoundDirectiveKeyword;
        break;

#define POUND_OBJECT_LITERAL(Name, Desc, Proto)
#define POUND_DIRECTIVE_KEYWORD(Name)
#define POUND_KEYWORD(Name) case tok::pound_##Name:
#include "swift/AST/TokenKinds.def"
        Kind = SyntaxNodeKind::Keyword;
        break;
      case tok::identifier:
        if (Tok.getText().startswith("<#"))
          Kind = SyntaxNodeKind::EditorPlaceholder;
        else
          Kind = SyntaxNodeKind::Identifier;
        break;
      case tok::dollarident: Kind = SyntaxNodeKind::DollarIdent; break;
      case tok::string_literal: Kind = SyntaxNodeKind::String; break;

      case tok::integer_literal:
        Kind = SyntaxNodeKind::Integer;
        if (UnaryMinusLoc.isValid()) {
          Loc = UnaryMinusLoc;
          Length = *Length + SM.getByteDistance(UnaryMinusLoc, Tok.getLoc());
        }
        break;
      case tok::floating_literal:
        Kind = SyntaxNodeKind::Floating;
        if (UnaryMinusLoc.isValid()) {
          Loc = UnaryMinusLoc;
          Length = *Length + SM.getByteDistance(UnaryMinusLoc, Tok.getLoc());
        }
        break;

      case tok::oper_prefix:
        if (Tok.getText() == "-" && I != E &&
            (Tokens[I+1].getKind() == tok::integer_literal ||
             Tokens[I+1].getKind() == tok::floating_literal)) {
          UnaryMinusLoc = Loc;
          continue;
        } else {
          Kind = SyntaxNodeKind::Operator;
          break;
        }
      case tok::oper_postfix:
      case tok::oper_binary_spaced:
      case tok::oper_binary_unspaced:
        Kind = SyntaxNodeKind::Operator;
        break;

      case tok::comment:
        if (Tok.getText().startswith("///") ||
            (IsPlayground && Tok.getText().startswith("//:")))
          Kind = SyntaxNodeKind::DocCommentLine;
        else if (Tok.getText().startswith("/**") ||
                 (IsPlayground && Tok.getText().startswith("/*:")))
          Kind = SyntaxNodeKind::DocCommentBlock;
        else if (Tok.getText().startswith("//"))
          Kind = SyntaxNodeKind::CommentLine;
        else
          Kind = SyntaxNodeKind::CommentBlock;
        break;
      case tok::at_sign:
        // Set the location of @ and continue. Next token should be the
        // attribute name.
        AttrLoc = Tok.getLoc();
        continue;

      case tok::string_interpolation_anchor: {
        Kind = SyntaxNodeKind::StringInterpolationAnchor;
        break;
      }

      case tok::unknown: {
        if (Tok.getRawText().ltrim('#').startswith("\"")) {
          // This is likely an invalid single-line ("), multi-line ("""),
          // or raw (#", ##", #""", etc.) string literal.
          Kind = SyntaxNodeKind::String;
          break;
        }
        continue;
      }

      default:
        continue;
      }
    }

    UnaryMinusLoc = SourceLoc(); // Reset.

    assert(Loc.isValid());
    assert(Nodes.empty() || SM.isBeforeInBuffer(Nodes.back().Range.getStart(),
                                                Loc));
    Nodes.emplace_back(Kind, CharSourceRange(Loc, Length.value()));
  }

  Impl.TokenNodes = std::move(Nodes);
}

SyntaxModelContext::~SyntaxModelContext() {
  delete &Impl;
}

namespace {

using ASTNodeType = ASTWalker::ParentTy;

struct StructureElement {
  SyntaxStructureNode StructureNode;
  ASTNodeType ASTNode;
  StructureElement(const SyntaxStructureNode &StructureNode,
                   const ASTNodeType &ASTNode)
    :StructureNode(StructureNode), ASTNode(ASTNode) { }
};

static const std::vector<std::string> URLProtocols = {

  // Use RegexStrURL:
  "acap", "afp", "afs", "cid", "data", "fax", "feed", "file", "ftp", "go",
  "gopher", "http", "https", "imap", "ldap", "mailserver", "mid", "modem",
  "news", "nntp", "opaquelocktoken", "pop", "prospero", "rdar", "rtsp", "service",
  "sip", "soap.beep", "soap.beeps", "tel", "telnet", "tip", "tn3270", "urn",
  "vemmi", "wais", "xcdoc", "z39.50r","z39.50s",

  // Use RegexStrMailURL:
  "mailto", "im",

  // Use RegexStrRadarURL:
  "radar"
};

static const char *const RegexStrURL =
  "(acap|afp|afs|cid|data|fax|feed|file|ftp|go|"
  "gopher|http|https|imap|ldap|mailserver|mid|modem|news|nntp|opaquelocktoken|"
  "pop|prospero|rdar|rtsp|service|sip|soap\\.beep|soap\\.beeps|tel|telnet|tip|"
  "tn3270|urn|vemmi|wais|xcdoc|z39\\.50r|z39\\.50s)://"
  "([a-zA-Z0-9\\-_.]+/)?[a-zA-Z0-9;/?:@\\&=+$,\\-_.!~*'()%#]+";

static const char *const RegexStrMailURL =
  "(mailto|im):[a-zA-Z0-9\\-_]+@[a-zA-Z0-9\\-_\\.!%]+";

static const char *const RegexStrRadarURL =
  "radar:[a-zA-Z0-9;/?:@\\&=+$,\\-_.!~*'()%#]+";

class ModelASTWalker : public ASTWalker {
  ArrayRef<Token> AllTokensInFile;
  const LangOptions &LangOpts;
  const SourceManager &SM;
  unsigned BufferID;
  ASTContext &Ctx;
  std::vector<StructureElement> SubStructureStack;
  SourceLoc LastLoc;
  static const std::regex &getURLRegex(StringRef Protocol);

  llvm::Optional<SyntaxNode> parseFieldNode(StringRef Text, StringRef OrigText,
                                            SourceLoc OrigLoc);
  llvm::DenseSet<ASTNode> NodesVisitedBefore;
  /// When non-zero, we should avoid passing tokens as syntax nodes since a parent of several tokens
  /// is considered as one, e.g. object literal expression.
  uint8_t AvoidPassingSyntaxToken = 0;

  class InactiveClauseRAII {
    const bool wasInInactiveClause;
    bool &isInInactiveClause;

  public:
    InactiveClauseRAII(bool &isInInactiveClauseArg, bool enteringInactiveClause)
        : wasInInactiveClause(isInInactiveClauseArg),
          isInInactiveClause(isInInactiveClauseArg) {
      isInInactiveClause |= enteringInactiveClause;
    }
    ~InactiveClauseRAII() { isInInactiveClause = wasInInactiveClause; }
  };
  friend class InactiveClauseRAII;
  bool inInactiveClause = false;

  struct ParentArgsTy {
    Expr *Parent = nullptr;
    llvm::DenseMap<Expr *, Argument> Args;
  };
  /// A mapping of argument expressions to their full argument info.
  SmallVector<ParentArgsTy, 4> ParentArgs;

public:
  SyntaxModelWalker &Walker;
  ArrayRef<SyntaxNode> TokenNodes;

  ModelASTWalker(const SourceFile &File, SyntaxModelWalker &Walker)
      : AllTokensInFile(File.getAllTokens()),
        LangOpts(File.getASTContext().LangOpts),
        SM(File.getASTContext().SourceMgr),
        BufferID(File.getBufferID().value()),
        Ctx(File.getASTContext()),
        Walker(Walker) { }

  // FIXME: Remove this
  bool shouldWalkAccessorsTheOldWay() override { return true; }

  /// Only walk the arguments of a macro, to represent the source as written.
  MacroWalking getMacroWalkingBehavior() const override {
    return MacroWalking::Arguments;
  }

  void visitSourceFile(SourceFile &SrcFile, ArrayRef<SyntaxNode> Tokens);

  PreWalkResult<ArgumentList *>
  walkToArgumentListPre(ArgumentList *ArgList) override;
  PostWalkResult<ArgumentList *>
  walkToArgumentListPost(ArgumentList *ArgList) override;

  PreWalkResult<Expr *> walkToExprPre(Expr *E) override;
  PostWalkResult<Expr *> walkToExprPost(Expr *E) override;
  PreWalkResult<Stmt *> walkToStmtPre(Stmt *S) override;
  PostWalkResult<Stmt *> walkToStmtPost(Stmt *S) override;
  PreWalkAction walkToDeclPre(Decl *D) override;
  PostWalkAction walkToDeclPost(Decl *D) override;
  PreWalkAction walkToTypeReprPre(TypeRepr *T) override;
  bool shouldWalkIntoGenericParams() override { return true; }

private:
  static bool findUrlStartingLoc(StringRef Text, unsigned &Start,
                                 std::regex& Regex);
  bool annotateIfConfigConditionIdentifiers(Expr *Cond);
  bool handleAttrs(const OrigDeclAttributes &Attrs);
  bool handleAttrs(const TypeAttributes &Attrs);

  using DeclAttributeAndRange = std::pair<const DeclAttribute *, SourceRange>;

  bool handleSpecialDeclAttribute(const DeclAttribute *Decl,
                                  ArrayRef<Token> Toks);
  bool handleAttrRanges(ArrayRef<DeclAttributeAndRange> DeclRanges);

  bool shouldPassBraceStructureNode(BraceStmt *S);

  enum PassNodesBehavior {
    /// Pass all nodes up to but not including the location.
    ExcludeNodeAtLocation,
    /// Pass all nodes up to and including the location.
    IncludeNodeAtLocation,
    /// Like ExcludeNodeAtLocation, and skip past any node at the location.
    DisplaceNodeAtLocation
  };
  struct PassUntilResult {
    bool shouldContinue;
    llvm::Optional<SyntaxNode> MatchedToken;
  };

  PassUntilResult
  passTokenNodesUntil(SourceLoc Loc, PassNodesBehavior Pass);
  bool passNonTokenNode(const SyntaxNode &Node);
  bool passNode(const SyntaxNode &Node);
  bool pushStructureNode(const SyntaxStructureNode &Node,
                         const ASTNodeType& ASTNode);
  bool popStructureNode();

  bool processComment(CharSourceRange Range);
  bool searchForURL(CharSourceRange Range);
  bool findFieldsInDocCommentLine(SyntaxNode Node);
  bool findFieldsInDocCommentBlock(SyntaxNode Node);
  bool isVisitedBefore(ASTNode Node) {
    return NodesVisitedBefore.count(Node) > 0;
  }
};

const std::regex &ModelASTWalker::getURLRegex(StringRef Pro) {
  static const std::regex Regexes[3] =  {
    std::regex{ RegexStrURL, std::regex::ECMAScript | std::regex::nosubs },
    std::regex{ RegexStrMailURL, std::regex::ECMAScript | std::regex::nosubs },
    std::regex{ RegexStrRadarURL, std::regex::ECMAScript | std::regex::nosubs }
  };

  static const auto MailToPosition = std::find(URLProtocols.begin(),
                                               URLProtocols.end(),
                                               "mailto");
  static const auto RadarPosition = std::find(URLProtocols.begin(),
                                              URLProtocols.end(),
                                              "radar");
  auto Found = std::find(URLProtocols.begin(), URLProtocols.end(), Pro);
  assert(Found != URLProtocols.end() && "bad protocol name");
  if (Found < MailToPosition)
    return Regexes[0];
  else if (Found < RadarPosition)
    return Regexes[1];
  else
    return Regexes[2];
}

SyntaxStructureKind syntaxStructureKindFromNominalTypeDecl(NominalTypeDecl *N) {
  if (isa<ClassDecl>(N))
    return SyntaxStructureKind::Class;
  else if (isa<StructDecl>(N))
    return SyntaxStructureKind::Struct;
  else if (isa<ProtocolDecl>(N))
    return SyntaxStructureKind::Protocol;
  else {
    // All other known NominalTypeDecl derived classes covered, so assert() here.
    assert(isa<EnumDecl>(N));
    return SyntaxStructureKind::Enum;
  }
}

CharSourceRange charSourceRangeFromSourceRange(const SourceManager &SM,
                                               const SourceRange &SR) {
  return Lexer::getCharSourceRangeFromSourceRange(SM, SR);
}

CharSourceRange innerCharSourceRangeFromSourceRange(const SourceManager &SM,
                                                    const SourceRange &SR) {
  if (SR.isInvalid())
    return CharSourceRange();

  SourceLoc SRS = Lexer::getLocForEndOfToken(SM, SR.Start);
  return CharSourceRange(SM, SRS, (SR.End != SR.Start) ? SR.End : SRS);
}

static void setDecl(SyntaxStructureNode &N, Decl *D) {
  N.Dcl = D;
  N.Attrs = D->getOriginalAttrs();
  N.DocRange = D->getRawComment().getCharSourceRange();
}

} // anonymous namespace

bool SyntaxModelContext::walk(SyntaxModelWalker &Walker) {
  ModelASTWalker ASTWalk(Impl.SrcFile, Walker);
  ASTWalk.visitSourceFile(Impl.SrcFile, Impl.TokenNodes);
  return true;
}

void ModelASTWalker::visitSourceFile(SourceFile &SrcFile,
                                     ArrayRef<SyntaxNode> Tokens) {
  TokenNodes = Tokens;
  SrcFile.walk(*this);

  // Pass the rest of the token nodes.
  for (auto &TokNode : TokenNodes)
    passNode(TokNode);
}

static bool shouldTreatAsSingleToken(const SyntaxStructureNode &Node,
                                     const SourceManager &SM) {
  // Avoid passing the individual syntax tokens corresponding to single-line
  // object literal expressions, as they will be reported as a single token.
  return Node.Kind == SyntaxStructureKind::ObjectLiteralExpression &&
         SM.getLineAndColumnInBuffer(Node.Range.getStart()).first ==
             SM.getLineAndColumnInBuffer(Node.Range.getEnd()).first;
}

ASTWalker::PreWalkResult<ArgumentList *>
ModelASTWalker::walkToArgumentListPre(ArgumentList *ArgList) {
  Expr *ParentExpr = Parent.getAsExpr();
  if (!ParentExpr)
    return Action::Continue(ArgList);

  ParentArgsTy Mapping;
  Mapping.Parent = ParentExpr;
  for (auto Arg : *ArgList) {
    auto res = Mapping.Args.try_emplace(Arg.getExpr(), Arg);
    assert(res.second && "Duplicate arguments?");
    (void)res;
  }
  ParentArgs.push_back(std::move(Mapping));
  return Action::Continue(ArgList);
}

ASTWalker::PostWalkResult<ArgumentList *>
ModelASTWalker::walkToArgumentListPost(ArgumentList *ArgList) {
  if (Expr *ParentExpr = Parent.getAsExpr()) {
    assert(ParentExpr == ParentArgs.back().Parent &&
           "Unmatched walkToArgumentList(Pre|Post)");
    ParentArgs.pop_back();
  }
  return Action::Continue(ArgList);
}

ASTWalker::PreWalkResult<Expr *> ModelASTWalker::walkToExprPre(Expr *E) {
  if (isVisitedBefore(E))
    return Action::SkipChildren(E);

  auto addCallArgExpr = [&](const Argument &Arg) {
    auto *Elem = Arg.getExpr();
    if (isa<DefaultArgumentExpr>(Elem))
      return;

    auto NL = Arg.getLabelLoc();
    auto Name = Arg.getLabel();

    SyntaxStructureNode SN;
    SN.Kind = SyntaxStructureKind::Argument;
    SN.BodyRange = charSourceRangeFromSourceRange(SM, Elem->getSourceRange());
    if (NL.isValid() && !Name.empty()) {
      SN.NameRange = CharSourceRange(NL, Name.getLength());
      SN.Range = charSourceRangeFromSourceRange(
          SM, SourceRange(NL, Elem->getEndLoc()));
      passTokenNodesUntil(NL, ExcludeNodeAtLocation);
    } else {
      SN.Range = SN.BodyRange;
    }

    pushStructureNode(SN, Elem);
  };

  if (auto *ParentExpr = Parent.getAsExpr()) {
    if (!ParentArgs.empty() && ParentArgs.back().Parent == ParentExpr) {
      auto &ArgumentInfo = ParentArgs.back().Args;
      auto Arg = ArgumentInfo.find(E);
      if (Arg != ArgumentInfo.end())
        addCallArgExpr(Arg->second);
    }
  }

  if (E->isImplicit())
    return Action::Continue(E);

  auto addExprElem = [&](const Expr *Elem, SyntaxStructureNode &SN) {
    if (isa<ErrorExpr>(Elem))
      return;
    SourceRange R = Elem->getSourceRange();
    if (R.isInvalid())
      return;
    SN.Elements.emplace_back(SyntaxStructureElementKind::Expr,
                             charSourceRangeFromSourceRange(SM, R));
  };

  if (auto *CE = dyn_cast<CallExpr>(E)) {
    SyntaxStructureNode SN;
    SN.Kind = SyntaxStructureKind::CallExpression;
    SN.Range = charSourceRangeFromSourceRange(SM, E->getSourceRange());
    if (CE->getFn() && CE->getFn()->getSourceRange().isValid())
      SN.NameRange = charSourceRangeFromSourceRange(SM,
                                                 CE->getFn()->getSourceRange());
    if (CE->getArgs()->getSourceRange().isValid())
      SN.BodyRange = innerCharSourceRangeFromSourceRange(
          SM, CE->getArgs()->getSourceRange());
    pushStructureNode(SN, CE);

  } else if (auto *SE = dyn_cast<SubscriptExpr>(E)) {
    SyntaxStructureNode SN;
    SN.Kind = SyntaxStructureKind::CallExpression;
    SN.Range = charSourceRangeFromSourceRange(SM, E->getSourceRange());
    SN.NameRange =
        charSourceRangeFromSourceRange(SM, SE->getBase()->getSourceRange());
    if (SE->getArgs()->getSourceRange().isValid())
      SN.BodyRange = innerCharSourceRangeFromSourceRange(
          SM, SE->getArgs()->getSourceRange());
    pushStructureNode(SN, SE);

  } else if (auto *ObjectE = dyn_cast<ObjectLiteralExpr>(E)) {
    SyntaxStructureNode SN;
    SN.Kind = SyntaxStructureKind::ObjectLiteralExpression;
    SN.Range = charSourceRangeFromSourceRange(SM, ObjectE->getSourceRange());
    SourceLoc NRStart = ObjectE->getSourceLoc().getAdvancedLoc(1);    
    SourceLoc NREnd =
      NRStart.getAdvancedLoc(ObjectE->getLiteralKindRawName().size());
    SN.NameRange = CharSourceRange(SM, NRStart, NREnd);
    SN.BodyRange =
      innerCharSourceRangeFromSourceRange(SM, ObjectE->getSourceRange());
    // Consider the object literal as a single syntax token for highlighting if
    // it spans a single line.
    if (shouldTreatAsSingleToken(SN, SM))
      passNonTokenNode({SyntaxNodeKind::ObjectLiteral, SN.Range});
    pushStructureNode(SN, E);
  } else if (auto *ArrayE = dyn_cast<ArrayExpr>(E)) {
    SyntaxStructureNode SN;
    SN.Kind = SyntaxStructureKind::ArrayExpression;
    SN.Range = charSourceRangeFromSourceRange(SM, E->getSourceRange());
    for (auto *Elem : ArrayE->getElements())
      addExprElem(Elem, SN);
    SN.BodyRange = innerCharSourceRangeFromSourceRange(SM, E->getSourceRange());
    pushStructureNode(SN, E);

  } else if (auto *DictE = dyn_cast<DictionaryExpr>(E)) {
    SyntaxStructureNode SN;
    SN.Kind = SyntaxStructureKind::DictionaryExpression;
    SN.Range = charSourceRangeFromSourceRange(SM, E->getSourceRange());
    for (auto *Elem : DictE->getElements()) {
      if (auto *TupleE = dyn_cast<TupleExpr>(Elem)) {
        for (auto *TE : TupleE->getElements())
          addExprElem(TE, SN);
      } else {
        addExprElem(Elem, SN);
      }
    }
    SN.BodyRange = innerCharSourceRangeFromSourceRange(SM, E->getSourceRange());
    pushStructureNode(SN, E);
  } else if (auto *Tup = dyn_cast<TupleExpr>(E)) {
    SyntaxStructureNode SN;
    SN.Kind = SyntaxStructureKind::TupleExpression;
    SN.Range = charSourceRangeFromSourceRange(SM, Tup->getSourceRange());
    SN.BodyRange =
        innerCharSourceRangeFromSourceRange(SM, Tup->getSourceRange());
    for (auto *Elem : Tup->getElements()) {
      addExprElem(Elem, SN);
    }
    pushStructureNode(SN, Tup);
  } else if (auto *Closure = dyn_cast<ClosureExpr>(E)) {
    SyntaxStructureNode SN;
    SN.Kind = SyntaxStructureKind::ClosureExpression;
    SN.Range = charSourceRangeFromSourceRange(SM, E->getSourceRange());
    SN.BodyRange = innerCharSourceRangeFromSourceRange(SM, E->getSourceRange());
    if (Closure->hasExplicitResultType())
      SN.TypeRange = charSourceRangeFromSourceRange(SM,
                          Closure->getExplicitResultTypeRepr()->getSourceRange());

    pushStructureNode(SN, Closure);

  } else if (auto SE = dyn_cast<SequenceExpr>(E)) {
    // In SequenceExpr, explicit cast expressions (e.g. 'as', 'is') appear
    // twice. Skip pointers we've already seen.
    SmallPtrSet<Expr *, 5> seenExpr;
    for (auto subExpr : SE->getElements()) {
      if (!seenExpr.insert(subExpr).second) {
        continue;
      }
      llvm::SaveAndRestore<ASTWalker::ParentTy> SetParent(Parent, E);
      subExpr->walk(*this);
    }
    // TODO: We should consider changing Action::SkipChildren to still call
    // walkToExprPost, which would eliminate the need for this.
    auto postWalkResult = walkToExprPost(SE);
    switch (postWalkResult.Action.Action) {
    case PostWalkAction::Stop:
      return Action::Stop();
    case PostWalkAction::Continue:
      // We already visited the children.
      return Action::SkipChildren(*postWalkResult.Value);
    }
    llvm_unreachable("Unhandled case in switch!");
  } else if (auto *ISL = dyn_cast<InterpolatedStringLiteralExpr>(E)) {
    // Don't visit the child expressions directly. Instead visit the arguments
    // of each appendStringLiteral/appendInterpolation CallExpr so we don't
    // try to output structure nodes for those calls.
    llvm::SaveAndRestore<ASTWalker::ParentTy> SetParent(Parent, E);
    ISL->forEachSegment(Ctx, [&](bool isInterpolation, CallExpr *CE) {
      if (isInterpolation) {
        for (auto arg : *CE->getArgs())
          arg.getExpr()->walk(*this);
      }
    });
    // TODO: We should consider changing Action::SkipChildren to still call
    // walkToExprPost, which would eliminate the need for this.
    auto postWalkResult = walkToExprPost(E);
    switch (postWalkResult.Action.Action) {
    case PostWalkAction::Stop:
      return Action::Stop();
    case PostWalkAction::Continue:
      // We already visited the children.
      return Action::SkipChildren(*postWalkResult.Value);
    }
    llvm_unreachable("Unhandled case in switch!");
  }

  return Action::Continue(E);
}

ASTWalker::PostWalkResult<Expr *> ModelASTWalker::walkToExprPost(Expr *E) {
  while (!SubStructureStack.empty() &&
      SubStructureStack.back().ASTNode.getAsExpr() == E)
    popStructureNode();

  return Action::Continue(E);
}

ASTWalker::PreWalkResult<Stmt *> ModelASTWalker::walkToStmtPre(Stmt *S) {
  if (isVisitedBefore(S)) {
    return Action::SkipChildren(S);
  }
  auto addExprElem = [&](SyntaxStructureElementKind K, const Expr *Elem,
                         SyntaxStructureNode &SN) {
    if (isa<ErrorExpr>(Elem))
      return;
    SourceRange R = Elem->getSourceRange();
    if (R.isInvalid())
      return;
    SN.Elements.emplace_back(K, charSourceRangeFromSourceRange(SM, R));
  };

  if (auto *ForEachS = dyn_cast<ForEachStmt>(S)) {
    SyntaxStructureNode SN;
    SN.Kind = SyntaxStructureKind::ForEachStatement;
    SN.Range = charSourceRangeFromSourceRange(SM, S->getSourceRange());
    if (ForEachS->getPattern()) {
      auto Pat = ForEachS->getPattern();
      if (!Pat->isImplicit()) {
        SourceRange ElemRange = Pat->getSourceRange();
        SN.Elements.emplace_back(SyntaxStructureElementKind::Id,
                                 charSourceRangeFromSourceRange(SM, ElemRange));
      }
    }
    if (auto *S = ForEachS->getParsedSequence())
      addExprElem(SyntaxStructureElementKind::Expr, S, SN);
    pushStructureNode(SN, S);

  } else if (auto *WhileS = dyn_cast<WhileStmt>(S)) {
    SyntaxStructureNode SN;
    SN.Kind = SyntaxStructureKind::WhileStatement;
    SN.Range = charSourceRangeFromSourceRange(SM, S->getSourceRange());
    if (!WhileS->getCond().empty()) {
      auto Conds = WhileS->getCond();
      SourceRange ElemRange = SourceRange(Conds.front().getSourceRange().Start,
                                          Conds.back().getSourceRange().End);
      SN.Elements.emplace_back(SyntaxStructureElementKind::ConditionExpr,
                               charSourceRangeFromSourceRange(SM, ElemRange));
    }
    pushStructureNode(SN, S);

  } else if (auto *RepeatWhileS = dyn_cast<RepeatWhileStmt>(S)) {
    SyntaxStructureNode SN;
    SN.Kind = SyntaxStructureKind::RepeatWhileStatement;
    SN.Range = charSourceRangeFromSourceRange(SM, S->getSourceRange());
    if (RepeatWhileS->getCond()) {
      addExprElem(SyntaxStructureElementKind::Expr, RepeatWhileS->getCond(), SN);
    }
    pushStructureNode(SN, S);

  } else if (auto *IfS = dyn_cast<IfStmt>(S)) {
    SyntaxStructureNode SN;
    SN.Kind = SyntaxStructureKind::IfStatement;
    SN.Range = charSourceRangeFromSourceRange(SM, S->getSourceRange());
    if (!IfS->getCond().empty()) {
      auto Conds = IfS->getCond();
      SourceRange ElemRange = SourceRange(Conds.front().getSourceRange().Start,
                                          Conds.back().getSourceRange().End);
      SN.Elements.emplace_back(SyntaxStructureElementKind::ConditionExpr,
                               charSourceRangeFromSourceRange(SM, ElemRange));
    }
    pushStructureNode(SN, S);

  } else if (auto *GS = dyn_cast<GuardStmt>(S)) {
    SyntaxStructureNode SN;
    SN.Kind = SyntaxStructureKind::GuardStatement;
    SN.Range = charSourceRangeFromSourceRange(SM, S->getSourceRange());
    if (!GS->getCond().empty()) {
      auto Conds = GS->getCond();
      SourceRange ElemRange = SourceRange(Conds.front().getSourceRange().Start,
                                          Conds.back().getSourceRange().End);
      SN.Elements.emplace_back(SyntaxStructureElementKind::ConditionExpr,
                               charSourceRangeFromSourceRange(SM, ElemRange));
    }
    pushStructureNode(SN, S);

  } else if (auto *SwitchS = dyn_cast<SwitchStmt>(S)) {
    SyntaxStructureNode SN;
    SN.Kind = SyntaxStructureKind::SwitchStatement;
    SN.Range = charSourceRangeFromSourceRange(SM, S->getSourceRange());
    if (SwitchS->getSubjectExpr()) {
      addExprElem(SyntaxStructureElementKind::Expr, SwitchS->getSubjectExpr(),
                  SN);
    }
    pushStructureNode(SN, S);

  } else if (auto *CaseS = dyn_cast<CaseStmt>(S)) {
    SyntaxStructureNode SN;
    SN.Kind = SyntaxStructureKind::CaseStatement;
    SN.Range = charSourceRangeFromSourceRange(SM, S->getSourceRange());
    for (const CaseLabelItem &Item : CaseS->getCaseLabelItems()) {
      SN.Elements.emplace_back(SyntaxStructureElementKind::Pattern,
                               charSourceRangeFromSourceRange(SM,
                                                        Item.getSourceRange()));
    }
    pushStructureNode(SN, S);

  } else if (isa<BraceStmt>(S) && shouldPassBraceStructureNode(cast<BraceStmt>(S))) {
    // Pass BraceStatement structure node.
    SyntaxStructureNode SN;
    SN.Kind = SyntaxStructureKind::BraceStatement;
    SN.Range = charSourceRangeFromSourceRange(SM, S->getSourceRange());
    SN.BodyRange = innerCharSourceRangeFromSourceRange(SM,
                                                       S->getSourceRange());
    pushStructureNode(SN, S);

  } else if (auto *SW = dyn_cast<SwitchStmt>(S)) {
    if (SW->getLBraceLoc().isValid() && SW->getRBraceLoc().isValid()) {
      SourceRange BraceRange(SW->getLBraceLoc(), SW->getRBraceLoc());
      SyntaxStructureNode SN;
      SN.Kind = SyntaxStructureKind::BraceStatement;
      SN.Range = charSourceRangeFromSourceRange(SM, BraceRange);
      SN.BodyRange = innerCharSourceRangeFromSourceRange(SM, BraceRange);
      pushStructureNode(SN, SW);
    }

  } else if (auto *DeferS = dyn_cast<DeferStmt>(S)) {
    // Since 'DeferStmt::getTempDecl()' is marked as implicit, we manually walk
    // into the body.
    if (auto *FD = DeferS->getTempDecl()) {
      if (auto *Body = FD->getBody()) {
        auto *RetS = Body->walk(*this);
        assert(RetS == Body);
        (void)RetS;
      }
      walkToStmtPost(DeferS);
    }
    // Already walked children.
    return Action::SkipChildren(DeferS);
  }

  return Action::Continue(S);
}

ASTWalker::PostWalkResult<Stmt *> ModelASTWalker::walkToStmtPost(Stmt *S) {
  while (!SubStructureStack.empty() &&
      SubStructureStack.back().ASTNode.getAsStmt() == S)
    popStructureNode();

  return Action::Continue(S);
}

ASTWalker::PreWalkAction ModelASTWalker::walkToDeclPre(Decl *D) {
  if (isVisitedBefore(D))
    return Action::SkipChildren();
  if (D->isImplicit())
    return Action::SkipChildren();

  // The attributes of EnumElementDecls and VarDecls are handled when visiting
  // their parent EnumCaseDecl/PatternBindingDecl (which the attributes are
  // attached to syntactically).
  if (!isa<EnumElementDecl>(D) &&
      !(isa<VarDecl>(D) && cast<VarDecl>(D)->getParentPatternBinding())) {
    if (!handleAttrs(D->getOriginalAttrs()))
      return Action::SkipChildren();
  }

  if (isa<AccessorDecl>(D)) {
    // Don't push structure nodes for accessors.
  } else if (auto *AFD = dyn_cast<AbstractFunctionDecl>(D)) {
    // Pass Function / Method structure node.
    SyntaxStructureNode SN;
    setDecl(SN, D);
    const DeclContext *DC = AFD->getDeclContext();
    auto *FD = dyn_cast<FuncDecl>(AFD);
    if (DC->isTypeContext()) {
      if (FD && FD->isStatic()) {
        if (FD->getStaticSpelling() == StaticSpellingKind::KeywordClass)
          SN.Kind = SyntaxStructureKind::ClassFunction;
        else
          SN.Kind = SyntaxStructureKind::StaticFunction;
      } else {
        SN.Kind = SyntaxStructureKind::InstanceFunction;
      }
    }
    else
      SN.Kind = SyntaxStructureKind::FreeFunction;
    SN.Range = charSourceRangeFromSourceRange(SM, AFD->getSourceRange());
    SN.BodyRange = innerCharSourceRangeFromSourceRange(SM,
                                                   AFD->getBodySourceRange());
    SN.NameRange = charSourceRangeFromSourceRange(SM,
                        AFD->getSignatureSourceRange());
    if (FD) {
      SN.TypeRange = charSourceRangeFromSourceRange(SM,
                                    FD->getResultTypeSourceRange());
    }
    pushStructureNode(SN, AFD);
  } else if (auto *NTD = dyn_cast<NominalTypeDecl>(D)) {
    SyntaxStructureNode SN;
    setDecl(SN, D);
    SN.Kind = syntaxStructureKindFromNominalTypeDecl(NTD);
    SN.Range = charSourceRangeFromSourceRange(SM, NTD->getSourceRange());
    SN.BodyRange = innerCharSourceRangeFromSourceRange(SM, NTD->getBraces());
    SourceLoc NRStart = NTD->getNameLoc();
    SourceLoc NREnd = NRStart.getAdvancedLoc(NTD->getName().getLength());
    SN.NameRange = CharSourceRange(SM, NRStart, NREnd);

    for (const TypeLoc &TL : NTD->getInherited()) {
      CharSourceRange TR = charSourceRangeFromSourceRange(SM,
                                                          TL.getSourceRange());
      SN.InheritedTypeRanges.push_back(TR);
      SN.Elements.emplace_back(SyntaxStructureElementKind::TypeRef, TR);
    }

    pushStructureNode(SN, NTD);

  } else if (auto *ED = dyn_cast<ExtensionDecl>(D)) {
    SyntaxStructureNode SN;
    setDecl(SN, D);
    SN.Kind = SyntaxStructureKind::Extension;
    SN.Range = charSourceRangeFromSourceRange(SM, ED->getSourceRange());
    SN.BodyRange = innerCharSourceRangeFromSourceRange(SM, ED->getBraces());
    SourceRange NSR = SourceRange();
    if (auto *repr = ED->getExtendedTypeRepr())
      NSR = repr->getSourceRange();
    SN.NameRange = charSourceRangeFromSourceRange(SM, NSR);

    for (const TypeLoc &TL : ED->getInherited()) {
      CharSourceRange TR = charSourceRangeFromSourceRange(SM,
                                                          TL.getSourceRange());
      SN.InheritedTypeRanges.push_back(TR);
      SN.Elements.emplace_back(SyntaxStructureElementKind::TypeRef, TR);
    }

    pushStructureNode(SN, ED);

  } else if (auto *PD = dyn_cast<ParamDecl>(D)) {
    SyntaxStructureNode SN;
    SN.Dcl = D;
    SN.Kind = SyntaxStructureKind::Parameter;
    if (!PD->getArgumentName().empty()) {
      SourceLoc ArgStart = PD->getSourceRange().Start;
      SN.NameRange = CharSourceRange(ArgStart, PD->getArgumentName().getLength());
      passTokenNodesUntil(ArgStart, PassNodesBehavior::ExcludeNodeAtLocation);
    }
    SN.Range = charSourceRangeFromSourceRange(SM, PD->getSourceRange());
    SN.Attrs = PD->getOriginalAttrs();
    SN.TypeRange = charSourceRangeFromSourceRange(SM,
                                      PD->getTypeSourceRangeForDiagnostics());
    pushStructureNode(SN, PD);
  } else if (auto *PBD = dyn_cast<PatternBindingDecl>(D)) {
    // Process the attributes of one of the contained VarDecls. Attributes that
    // are syntactically attached to the PatternBindingDecl end up on the
    // contained VarDecls.
    VarDecl *Contained = nullptr;
    for (auto idx : range(PBD->getNumPatternEntries())) {
      PBD->getPattern(idx)->forEachVariable([&](VarDecl *VD) -> void {
        Contained = VD;
      });
      if (Contained) {
        if (!handleAttrs(Contained->getOriginalAttrs()))
          return Action::SkipChildren();
        break;
      }
    }
  } else if (auto *VD = dyn_cast<VarDecl>(D)) {
    const DeclContext *DC = VD->getDeclContext();
    SyntaxStructureNode SN;
    setDecl(SN, D);
    SourceRange SR;
    if (auto *PBD = VD->getParentPatternBinding())
      SR = PBD->getSourceRange();
    else
      SR = VD->getSourceRange();
    SN.Range = charSourceRangeFromSourceRange(SM, SR);
    auto bracesRange = VD->getBracesRange();
    if (bracesRange.isValid())
      SN.BodyRange = innerCharSourceRangeFromSourceRange(SM, bracesRange);
    SourceLoc NRStart = VD->getNameLoc();
    SourceLoc NREnd = (!VD->getName().empty()
                       ? NRStart.getAdvancedLoc(VD->getName().getLength())
                       : NRStart);
    SN.NameRange = CharSourceRange(SM, NRStart, NREnd);
    SN.TypeRange = charSourceRangeFromSourceRange(SM,
                                        VD->getTypeSourceRangeForDiagnostics());

    if (DC->isLocalContext()) {
      SN.Kind = SyntaxStructureKind::LocalVariable;
    } else if (DC->isTypeContext()) {
      if (VD->isStatic()) {
        StaticSpellingKind Spell = StaticSpellingKind::KeywordStatic;
        if (auto *PBD = VD->getParentPatternBinding())
          Spell = PBD->getStaticSpelling();
        if (Spell == StaticSpellingKind::KeywordClass)
          SN.Kind = SyntaxStructureKind::ClassVariable;
        else
          SN.Kind = SyntaxStructureKind::StaticVariable;
      } else {
        SN.Kind = SyntaxStructureKind::InstanceVariable;
      }
    } else {
      SN.Kind = SyntaxStructureKind::GlobalVariable;
    }
    pushStructureNode(SN, VD);

  } else if (auto *ConfigD = dyn_cast<IfConfigDecl>(D)) {
    for (auto &Clause : ConfigD->getClauses()) {
      if (Clause.Cond && !annotateIfConfigConditionIdentifiers(Clause.Cond))
        return Action::SkipChildren();

      InactiveClauseRAII inactiveClauseRAII(inInactiveClause, !Clause.isActive);
      for (auto &Element : Clause.Elements) {
        if (auto *E = Element.dyn_cast<Expr*>()) {
          E->walk(*this);
        } else if (auto *S = Element.dyn_cast<Stmt*>()) {
          S->walk(*this);
        } else {
          Element.get<Decl*>()->walk(*this);
        }
        NodesVisitedBefore.insert(Element);
      }
    }

  } else if (auto *EnumCaseD = dyn_cast<EnumCaseDecl>(D)) {
    SyntaxStructureNode SN;
    setDecl(SN, D);
    SN.Kind = SyntaxStructureKind::EnumCase;
    SN.Range = charSourceRangeFromSourceRange(SM, D->getSourceRange());

    // We need to handle the special case where attributes semantically
    // attach to enum element decls while syntactically locate before enum case decl.
    if (auto *element = EnumCaseD->getFirstElement()) {
      if (!handleAttrs(element->getOriginalAttrs()))
        return Action::SkipChildren();
    }
    if (pushStructureNode(SN, D)) {
      // FIXME: ASTWalker walks enum elements as members of the enum decl, not
      // as members of the enum case decl. Walk them manually here so that they
      // end up as child nodes of enum case.
      for (auto *EnumElemD : EnumCaseD->getElements()) {
        if (EnumElemD->getBaseIdentifier().empty())
          continue;
        SyntaxStructureNode SN;
        setDecl(SN, EnumElemD);
        SN.Kind = SyntaxStructureKind::EnumElement;
        SN.Range = charSourceRangeFromSourceRange(SM,
                                                  EnumElemD->getSourceRange());
        if (auto ParamList = EnumElemD->getParameterList()) {
          SourceRange NameRange = SourceRange(EnumElemD->getNameLoc(),
                                              ParamList->getSourceRange().End);
          SN.NameRange = charSourceRangeFromSourceRange(SM, NameRange);
        } else {
          SN.NameRange = CharSourceRange(EnumElemD->getNameLoc(),
                                         EnumElemD->getBaseIdentifier()
                                           .getLength());
        }

        if (auto *E = EnumElemD->getRawValueUnchecked()) {
          SourceRange ElemRange = E->getSourceRange();
          SN.Elements.emplace_back(SyntaxStructureElementKind::InitExpr,
                                 charSourceRangeFromSourceRange(SM, ElemRange));
        }
        pushStructureNode(SN, EnumElemD);
        EnumElemD->walk(*this);
        NodesVisitedBefore.insert(EnumElemD);
      }
    }
  } else if (auto *TypeAliasD = dyn_cast<TypeAliasDecl>(D)) {
    SyntaxStructureNode SN;
    setDecl(SN, D);
    SN.Kind = SyntaxStructureKind::TypeAlias;
    SN.Range = charSourceRangeFromSourceRange(SM,
                                              TypeAliasD->getSourceRange());
    SN.NameRange = CharSourceRange(TypeAliasD->getNameLoc(),
                                   TypeAliasD->getName().getLength());
    pushStructureNode(SN, TypeAliasD);
  } else if (auto *SubscriptD = dyn_cast<SubscriptDecl>(D)) {
    SyntaxStructureNode SN;
    setDecl(SN, D);
    SN.Kind = SyntaxStructureKind::Subscript;
    SN.Range = charSourceRangeFromSourceRange(SM,
                                              SubscriptD->getSourceRange());
    SN.BodyRange = innerCharSourceRangeFromSourceRange(SM,
                                               SubscriptD->getBracesRange());
    SN.NameRange = charSourceRangeFromSourceRange(SM,
                                        SubscriptD->getSignatureSourceRange());
    SN.TypeRange = charSourceRangeFromSourceRange(SM,
                            SubscriptD->getElementTypeSourceRange());
    pushStructureNode(SN, SubscriptD);
  } else if (auto *AssociatedTypeD = dyn_cast<AssociatedTypeDecl>(D)) {
    SyntaxStructureNode SN;
    setDecl(SN, D);
    SN.Kind = SyntaxStructureKind::AssociatedType;
    SN.Range = charSourceRangeFromSourceRange(SM,
                                            AssociatedTypeD->getSourceRange());
    SN.NameRange = CharSourceRange(AssociatedTypeD->getNameLoc(),
                                   AssociatedTypeD->getName().getLength());
    pushStructureNode(SN, AssociatedTypeD);
  } else if (auto *GenericParamD = dyn_cast<GenericTypeParamDecl>(D)) {
    SyntaxStructureNode SN;
    setDecl(SN, D);
    SN.Kind = SyntaxStructureKind::GenericTypeParam;
    SN.Range = charSourceRangeFromSourceRange(SM,
                                              GenericParamD->getSourceRange());
    SN.NameRange = CharSourceRange(GenericParamD->getNameLoc(),
                                   GenericParamD->getName().getLength());
    for (const TypeLoc &TL : GenericParamD->getInherited()) {
      CharSourceRange TR = charSourceRangeFromSourceRange(SM,
                                                          TL.getSourceRange());
      SN.InheritedTypeRanges.push_back(TR);
      SN.Elements.emplace_back(SyntaxStructureElementKind::TypeRef, TR);
    }

    pushStructureNode(SN, GenericParamD);
  }

  return Action::Continue();
}

ASTWalker::PostWalkAction ModelASTWalker::walkToDeclPost(swift::Decl *D) {
  while (!SubStructureStack.empty() &&
      SubStructureStack.back().ASTNode.getAsDecl() == D)
    popStructureNode();

  return Action::Continue();
}

ASTWalker::PreWalkAction ModelASTWalker::walkToTypeReprPre(TypeRepr *T) {
  if (auto AttrT = dyn_cast<AttributedTypeRepr>(T)) {
    if (!handleAttrs(AttrT->getAttrs()))
      return Action::SkipChildren();

  } else if (auto IdT = dyn_cast<IdentTypeRepr>(T)) {
    if (!passTokenNodesUntil(IdT->getStartLoc(),
                             ExcludeNodeAtLocation).shouldContinue)
      return Action::SkipChildren();
    if (TokenNodes.empty() ||
        TokenNodes.front().Range.getStart() != IdT->getStartLoc())
      return Action::SkipChildren();
    if (!passNode({SyntaxNodeKind::TypeId, TokenNodes.front().Range}))
      return Action::SkipChildren();
    TokenNodes = TokenNodes.slice(1);
  }
  return Action::Continue();
}

namespace {
template <typename FnTy>
class IdRefWalker : public ASTWalker {
  const FnTy &Fn;

public:
  IdRefWalker(const FnTy &Fn) : Fn(Fn) {}

  MacroWalking getMacroWalkingBehavior() const override {
    return MacroWalking::ArgumentsAndExpansion;
  }

  PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
    if (auto DRE = dyn_cast<UnresolvedDeclRefExpr>(E)) {
      if (!DRE->hasName())
        return Action::Continue(E);
      if (DRE->getRefKind() != DeclRefKind::Ordinary)
        return Action::Continue(E);
      if (!Fn(CharSourceRange(
              DRE->getSourceRange().Start,
              DRE->getName().getBaseName().userFacingName().size())))
        return Action::Stop();
    }
    return Action::Continue(E);
  }
};
} // end anonymous namespace

bool ModelASTWalker::annotateIfConfigConditionIdentifiers(Expr *Cond) {
  if (!Cond)
    return true;
  auto passNode = [&](CharSourceRange R) {
    return passNonTokenNode({ SyntaxNodeKind::BuildConfigId, R });
  };

  IdRefWalker<decltype(passNode)> Walker(passNode);
  return Cond->walk(Walker);
}

bool ModelASTWalker::handleSpecialDeclAttribute(const DeclAttribute *D,
                                                ArrayRef<Token> Toks) {
  if (!D)
    return false;
  if (isa<CustomAttr>(D) || isa<AvailableAttr>(D)) {
    if (!passTokenNodesUntil(D->getRangeWithAt().Start,
                             ExcludeNodeAtLocation).shouldContinue)
      return false;
    if (auto *CA = dyn_cast<CustomAttr>(D)) {
      if (auto *Repr = CA->getTypeRepr()) {
        if (!Repr->walk(*this))
          return false;
      }
      if (auto *Args = CA->getArgs()) {
        if (!Args->walk(*this))
          return false;
      }
    } else if (!TokenNodes.empty()) {
      auto Next = TokenNodes.front();
      if (Next.Range.getStart() == D->getRangeWithAt().Start) {
        TokenNodes = TokenNodes.drop_front();
        if (!passNode({SyntaxNodeKind::AttributeBuiltin, Next.Range}))
          return false;
      } else {
        // Only mispelled attributes, corrected in the AST but not
        // recognised or present in TokenNodes should get us here.
        // E.g. @availability(...) comes through as if @available(...) was
        // specified, but there's no TokenNode because we don't highlight them
        // (to indicate they're invalid).
        assert(Next.Range.getStart() == D->getRange().Start &&
               "Attribute's TokenNodes already consumed?");
      }
    } else {
        assert(0 && "No TokenNodes?");
    }
    if (!passTokenNodesUntil(D->getRange().End,
                             IncludeNodeAtLocation).shouldContinue)
      return false;
    return true;
  }
  if (isa<RethrowsAttr>(D))
    return true;
  return false;
}

bool ModelASTWalker::handleAttrs(const OrigDeclAttributes &Attrs) {
  SmallVector<DeclAttributeAndRange, 4> DeclRanges;
  for (auto *At : Attrs) {
    if (At->getRangeWithAt().isValid())
      DeclRanges.push_back(std::make_pair(At, At->getRangeWithAt()));
  }
  return handleAttrRanges(DeclRanges);
}

bool ModelASTWalker::handleAttrs(const TypeAttributes &Attrs) {
  SmallVector<SourceLoc, 4> AttrLocs;
  Attrs.getAttrLocs(AttrLocs);
  SmallVector<DeclAttributeAndRange, 4> DeclRanges;
  for (auto AttrLoc : AttrLocs) {
    DeclRanges.push_back(std::make_pair(nullptr, SourceRange(AttrLoc)));
  }
  return handleAttrRanges(DeclRanges);
}

bool ModelASTWalker::handleAttrRanges(ArrayRef<DeclAttributeAndRange> DeclRanges) {
  if (DeclRanges.empty())
    return true;

  SmallVector<DeclAttributeAndRange, 4> SortedRanges(DeclRanges.begin(),
                                                     DeclRanges.end());
  std::sort(
      SortedRanges.begin(), SortedRanges.end(),
      [&](DeclAttributeAndRange LHS, DeclAttributeAndRange RHS) {
        // Since attributes don't overlap it's safe to compare just by the
        // range's Start
        return SM.isBeforeInBuffer(LHS.second.Start, RHS.second.Start);
      });
  // Handle duplicate synthesized attributes due to * in @available
  auto NewEnd = std::unique(SortedRanges.begin(), SortedRanges.end(),
                            [&](DeclAttributeAndRange LHS, DeclAttributeAndRange RHS) {
                              return LHS.second == RHS.second;
                            });
  if (NewEnd != SortedRanges.end())
    SortedRanges.erase(NewEnd, SortedRanges.end());
  DeclRanges = SortedRanges;

  SourceLoc BeginLoc = DeclRanges.front().second.Start;
  auto Toks = slice_token_array(AllTokensInFile, BeginLoc,
                                DeclRanges.back().second.End);

  auto passAttrNode = [&](SourceRange AttrRange) -> bool {
    SourceRange Range = AttrRange;
    auto PassUntilResult = passTokenNodesUntil(Range.Start,
                                               ExcludeNodeAtLocation);
    if (!PassUntilResult.shouldContinue)
      return false;

    if (PassUntilResult.MatchedToken) {
      // Type attribute ranges don't have the correct end location (it only
      // covers the @ itself), so use matched token's range instead.
      CharSourceRange AdjustedRange = charSourceRangeFromSourceRange(SM, Range);
      AdjustedRange.widen(PassUntilResult.MatchedToken->Range);
      if (!passNode({SyntaxNodeKind::AttributeBuiltin, AdjustedRange}))
        return false;
      TokenNodes = TokenNodes.drop_while([&](SyntaxNode TokenNode) {
        return AdjustedRange.contains(TokenNode.Range.getStart());
      });
    } else {
        // Make sure we're revisiting something, rather than dealing with bad
        // source locations
        assert((TokenNodes.empty() ||
                SM.isBeforeInBuffer(AttrRange.End,
                                    TokenNodes.front().Range.getStart())) &&
               "AttrRange doesn't align with any TokenNode?");
    }
    return true;
  };

  for (auto Tok : Toks) {
    if (DeclRanges.empty())
      break;
    if (Tok.getLoc() == DeclRanges.front().second.Start) {
      auto R = DeclRanges.front().second;
      auto D = DeclRanges.front().first;
      DeclRanges = DeclRanges.slice(1);
      if (!handleSpecialDeclAttribute(D, Toks)) {
        if (!passAttrNode(R))
          return false;
      }
    }
  }

  if (!DeclRanges.empty() &&
      !handleSpecialDeclAttribute(DeclRanges.front().first, Toks)) {
    if (!passAttrNode(DeclRanges.front().second))
      return false;
  }

  return true;
}

bool ModelASTWalker::shouldPassBraceStructureNode(BraceStmt *S) {
  return (!dyn_cast_or_null<AbstractFunctionDecl>(Parent.getAsDecl()) &&
          !dyn_cast_or_null<TopLevelCodeDecl>(Parent.getAsDecl()) &&
          !dyn_cast_or_null<CaseStmt>(Parent.getAsStmt()) &&
          S->getSourceRange().isValid() &&
          !S->isImplicit());
}

ModelASTWalker::PassUntilResult
ModelASTWalker::passTokenNodesUntil(SourceLoc Loc,
                                    PassNodesBehavior Behavior) {
  assert(Loc.isValid());
  unsigned I = 0;
  llvm::Optional<SyntaxNode> MatchedToken;
  for (unsigned E = TokenNodes.size(); I != E; ++I) {
    SourceLoc StartLoc = TokenNodes[I].Range.getStart();
    if (SM.isBeforeInBuffer(Loc, StartLoc)) {
      break;
    }
    if (StartLoc == Loc) {
      MatchedToken = TokenNodes[I];
      if (Behavior != IncludeNodeAtLocation) {
        if (Behavior == DisplaceNodeAtLocation) {
          // Skip past the node directly at the specified location, allowing the
          // caller to effectively replace it.
          ++I;
        }
        break;
      }
    }
    if (!AvoidPassingSyntaxToken) {
      if (!passNode(TokenNodes[I]))
        return {false, llvm::None};
    }
  }

  TokenNodes = TokenNodes.slice(I);
  return {true, MatchedToken};
}

bool ModelASTWalker::passNonTokenNode(const SyntaxNode &Node) {
  // Skip out of order non-token nodes.
  // Ideally this shouldn't happen, but the AST can contain overlapping nodes,
  // such as multiple PatternBindingDecl in code like: var a, b : Int. Which
  // would cause us to report the TypeRepr twice.
  if (!SM.isBeforeInBuffer(LastLoc, Node.Range.getStart()))
    return true;

  if (!passTokenNodesUntil(Node.Range.getStart(),
                           DisplaceNodeAtLocation).shouldContinue)
    return false;
  if (!passNode(Node))
    return false;
  return true;
}

bool ModelASTWalker::passNode(const SyntaxNode &Node) {
  assert(!SM.isBeforeInBuffer(Node.Range.getStart(), LastLoc));
  LastLoc = Node.Range.getStart();

  bool ShouldWalkSubTree = Walker.walkToNodePre(Node);

  if (ShouldWalkSubTree) {
    if (Node.isComment()) {
      if (!processComment(Node.Range))
        return false;
    } else if (Node.Kind == SyntaxNodeKind::DocCommentLine) {
      if (!findFieldsInDocCommentLine(Node))
        return false;
    } else if (Node.Kind == SyntaxNodeKind::DocCommentBlock) {
      if (!findFieldsInDocCommentBlock(Node))
        return false;
    } else if (Node.Kind == SyntaxNodeKind::CommentMarker) {
      if (!searchForURL(Node.Range))
        return false;
    }
  }

  return Walker.walkToNodePost(Node);
}

bool ModelASTWalker::pushStructureNode(const SyntaxStructureNode &Node,
                                       const ASTNodeType& ASTNode) {
  SubStructureStack.emplace_back(Node, ASTNode);
  if (shouldTreatAsSingleToken(Node, SM))
    ++AvoidPassingSyntaxToken;

  if (!passTokenNodesUntil(Node.Range.getStart(),
                           ExcludeNodeAtLocation).shouldContinue)
    return false;
  if (!Walker.walkToSubStructurePre(Node))
    return false;

  return true;
}

bool ModelASTWalker::popStructureNode() {
  assert(!SubStructureStack.empty());
  SyntaxStructureNode Node = SubStructureStack.back().StructureNode;
  SWIFT_DEFER {
    if (shouldTreatAsSingleToken(Node, SM)) {
      assert(AvoidPassingSyntaxToken);
      AvoidPassingSyntaxToken --;
    }
  };
  SubStructureStack.pop_back();

  // VarDecls are popped before we see their TypeRepr, so if we pass the token
  // nodes now they will not change from identifier to a type-identifier.
  if (!Node.hasSubstructure()) {
    if (!passTokenNodesUntil(Node.Range.getEnd(),
                             IncludeNodeAtLocation).shouldContinue)
      return false;
  }
  if (!Walker.walkToSubStructurePost(Node))
    return false;

  return true;
}

bool ModelASTWalker::processComment(CharSourceRange Range) {
  StringRef Text = SM.extractText(Range, BufferID);
  SourceLoc Loc = Range.getStart();
  // Search for 'FIXME:' or 'TODO:'.
  while (1) {
    auto Pos = Text.find_first_of("FTM");
    if (Pos == StringRef::npos)
      return searchForURL(Range);

    Text = Text.substr(Pos);
    Loc = Loc.getAdvancedLoc(Pos);
    if (Text.startswith("FIXME:") || Text.startswith("TODO:") ||
        Text.startswith("MARK:"))
      break;
    Text = Text.substr(1);
    Loc = Loc.getAdvancedLoc(1);
  }

  auto NewLinePos = Text.find_first_of("\r\n");
  if (NewLinePos != StringRef::npos) {
    Text = Text.substr(0, NewLinePos);
  }
  if (Text.endswith("*/")) {
    Text = Text.drop_back(2);
  }
  Text = Text.rtrim();

  CharSourceRange BeforeMarker{ SM, Range.getStart(), Loc };
  CharSourceRange Marker(Loc, Text.size());
  CharSourceRange AfterMarker{ SM, Marker.getEnd(), Range.getEnd() };

  if (!searchForURL(BeforeMarker))
    return false;

  SyntaxNode Node{ SyntaxNodeKind::CommentMarker, Marker };
  if (!passNode(Node))
    return false;

  return searchForURL(AfterMarker);  
}

bool ModelASTWalker::findUrlStartingLoc(StringRef Text,
                                        unsigned &Start,
                                        std::regex &Regex) {
  static const auto MailToPosition = std::find(URLProtocols.begin(),
                                               URLProtocols.end(),
                                               "mailto");
  auto Index = Text.find(":");
  if (Index == StringRef::npos)
    return false;

  auto Lookback = [Text](unsigned Index, StringRef Name) {
    return Index >= Name.size() &&
      Text.substr(Index - Name.size(), Name.size()) == Name;
  };

  auto HasSlash = Text.substr(Index).startswith("://");

  if (HasSlash) {
    for (auto It = URLProtocols.begin(); It < URLProtocols.end(); ++ It) {
      if (Lookback(Index, *It)) {
        Regex = getURLRegex(*It);
        Start = Index - It->size();
        return true;
      }
    }
  } else {
    for (auto It = MailToPosition; It < URLProtocols.end(); ++ It) {
      if (Lookback(Index, *It)) {
        Regex = getURLRegex(*It);
        Start = Index - It->size();
        return true;
      }
    }
  }
  return false;
}

static CharSourceRange sanitizeUnpairedParenthesis(CharSourceRange Range) {
  auto Text = Range.str();
  if (Text.back() != ')') {
    return Range;
  }
  unsigned Pairs = 0;
  unsigned TrimLen = 0;
  for (char C : Text) {
    if (C == '(') {
      ++Pairs;
    } else if (C == ')') {
      if (Pairs == 0)
        ++TrimLen;
      else
        --Pairs;
    } else {
      TrimLen = 0;
    }
  }

  return CharSourceRange(Range.getStart(), Text.size() - TrimLen);
}

bool ModelASTWalker::searchForURL(CharSourceRange Range) {
  StringRef OrigText = SM.extractText(Range, BufferID);
  SourceLoc OrigLoc = Range.getStart();

  StringRef Text = OrigText;
  while (1) {
    std::match_results<StringRef::iterator> Matches;
    std::regex Regex;
    unsigned Start;
    if (findUrlStartingLoc(Text, Start, Regex) &&
        std::regex_search(Text.substr(Start).begin(),
                          Text.substr(Start).end(), Matches, Regex)) {
      auto &RxMatch = Matches[0];
      StringRef Match(RxMatch.first, RxMatch.second - RxMatch.first);
      SourceLoc Loc = OrigLoc.getAdvancedLoc(Match.data() - OrigText.data());
      CharSourceRange Range(Loc, Match.size());
      SyntaxNode Node{ SyntaxNodeKind::CommentURL,
                       sanitizeUnpairedParenthesis(Range) };
      if (!passNode(Node))
        return false;
      Text = Text.substr(Match.data() - Text.data() + Match.size());
    } else {
      auto Index = Text.find(':');
      if (Index == StringRef::npos)
        break;
      Text = Text.substr(Index + 1);
    }
  }
  return true;
}

namespace {
class DocFieldParser {
  const char *ptr;
  const char *end;

  bool advanceIf(char c) {
    if (ptr == end || c != *ptr)
      return false;
    ++ptr;
    return true;
  }
  bool advanceIf(llvm::function_ref<bool(char)> predicate) {
    if (ptr == end || !predicate(*ptr))
      return false;
    ++ptr;
    return true;
  }

public:
  DocFieldParser(StringRef text) : ptr(text.begin()), end(text.end()) {
    assert(!text.rtrim().contains('\n') && "expected single line");
  }

  // Case-insensitively match one of the following patterns:
  // ^[ ]?- (parameter) [^:]*:
  // ^[ ]?- (Parameters):
  // ^[ ]*- (...MarkupSimpleFields.def...|returns):
  llvm::Optional<StringRef> parseFieldName() {
    unsigned numSpaces = 0;
    while (advanceIf(' '))
      ++numSpaces;
    if (!advanceIf('-') || !advanceIf(' '))
      return llvm::None;

    if (ptr == end || !clang::isAsciiIdentifierContinue(*ptr))
      return llvm::None;
    const char *identStart = ptr++;
    while (advanceIf([](char c) { return clang::isAsciiIdentifierContinue(c); }))
      ;
    StringRef ident(identStart, ptr - identStart);

    if (ident.equals_insensitive("parameter")) {
      if (numSpaces > 1 || !advanceIf(' '))
        return llvm::None;
      while (advanceIf([](char c) { return c != ':'; }))
        ;
      if (!advanceIf(':'))
        return llvm::None;
      return ident;

    } else if (advanceIf(':')) {
      if (ident.equals_insensitive("parameters") && numSpaces > 1)
        return llvm::None;
      auto lowerIdent = ident.lower();
      bool isField = llvm::StringSwitch<bool>(lowerIdent)
#define MARKUP_SIMPLE_FIELD(Id, Keyword, XMLKind) .Case(#Keyword, true)
#include "swift/Markup/SimpleFields.def"
                         .Case("parameters", true)
                         .Case("returns", true)
                         .Default(false);
      if (isField)
        return ident;
    }

    return llvm::None;
  }
};
} // end anonymous namespace

llvm::Optional<SyntaxNode> ModelASTWalker::parseFieldNode(StringRef Text,
                                                          StringRef OrigText,
                                                          SourceLoc OrigLoc) {
  llvm::Optional<SyntaxNode> Node;
  DocFieldParser parser(Text);
  if (auto ident = parser.parseFieldName()) {
    auto loc = OrigLoc.getAdvancedLoc(ident->data() - OrigText.data());
    CharSourceRange range(loc, ident->size());
    Node = llvm::Optional<SyntaxNode>({SyntaxNodeKind::DocCommentField, range});
  }
  return Node;
}

bool ModelASTWalker::findFieldsInDocCommentLine(SyntaxNode Node) {
  auto OrigText = SM.extractText(Node.Range, BufferID);
  auto OrigLoc = Node.Range.getStart();

  auto Text = OrigText.drop_front(3); // Drop "///"
  if (Text.empty())
    return true;

  auto FieldNode = parseFieldNode(Text, OrigText, OrigLoc);
  if (FieldNode.has_value())
    passNode(FieldNode.value());
  else
    searchForURL(Node.Range);
  return true;
}

bool ModelASTWalker::findFieldsInDocCommentBlock(SyntaxNode Node) {
  auto OrigText = SM.extractText(Node.Range, BufferID);
  auto OrigLoc = Node.Range.getStart();

  if (!OrigText.startswith("/**") &&
      !(LangOpts.Playground && OrigText.startswith("/*:")))
    return true;

  auto Text = OrigText.drop_front(3); // Drop "^/**" or "/*:"

  if (!Text.endswith("*/"))
    return true;

  Text = Text.drop_back(2); // Drop "*/"

  if (Text.empty())
    return true;

  llvm::SmallVector<StringRef, 8> RawLines;
  Text.split(RawLines, '\n');
  auto FirstNewLine = std::find_if(RawLines.begin(), RawLines.end(),
    [](StringRef Line) { return !Line.trim().empty(); });

  if (FirstNewLine == RawLines.end())
    return true;

  Text = Text.substr(FirstNewLine->data() - Text.data());
  if (Text.empty())
    return true;

  size_t Indent = Text.ltrim().data() - Text.data();
  SmallVector<StringRef, 10> Lines;
  Text.split(Lines, "\n");

  for (auto Line : Lines) {
    Line = Line.rtrim();
    if (Line.size() < Indent)
      continue;
    auto FieldNode = parseFieldNode(Line.drop_front(Indent), OrigText, OrigLoc);
    if (FieldNode.has_value())
      passNode(FieldNode.value());
    else
      searchForURL(CharSourceRange(Node.Range.getStart().
        getAdvancedLoc(Line.data() - OrigText.data()),
                                   Line.size()));
  }

  std::match_results<StringRef::iterator> Matches;
  return true;
}
