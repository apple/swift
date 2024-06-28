//===--- Markup.cpp - Markup ----------------------------------------------===//
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

#include "swift/Markup/Markup.h"
#include "cmark-gfm.h"
#include "swift/AST/Comment.h"
#include "swift/Basic/Assertions.h"
#include "swift/Markup/LineList.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/ErrorHandling.h"
#include <optional>

using namespace swift;
using namespace markup;

struct ParseState {
  cmark_iter *Iter = nullptr;
  cmark_event_type Event = CMARK_EVENT_NONE;
  cmark_node *Node = nullptr;

public:
  ParseState() = default;
  ParseState(cmark_iter *Iter, cmark_event_type Event = CMARK_EVENT_NONE,
             cmark_node *Node = nullptr)
    : Iter(Iter), Event(Event), Node(Node) {}

  ParseState next() const {
    auto I = Iter;
    auto Event = cmark_iter_next(I);
    auto Node = cmark_iter_get_node(I);
    return ParseState(I, Event, Node);
  }

  cmark_node_type getType() const {
    return cmark_node_get_type(Node);
  }
};

template <typename NodeType>
struct ParseResult {
  NodeType *Node;
  ParseState State;

public:
  operator ParseResult<MarkupASTNode>() {
    return { cast<MarkupASTNode>(Node), State };
  }
};

StringRef getLiteralContent(MarkupContext &MC, cmark_node *Node) {
  // Literal content nodes never have start/end column line information.
  // It is a floating piece of text that inherits location information from
  // its parent.
  auto Literal = cmark_node_get_literal(Node);
  assert(Literal != nullptr);
  return MC.allocateCopy(StringRef(Literal));
}

ParseResult<MarkupASTNode> parseElement(MarkupContext &MC, ParseState State);

ParseState parseChildren(MarkupContext &MC, ParseState State,
                         SmallVectorImpl<MarkupASTNode *> &Children) {
  auto Root = State.Node;
  State = State.next();
  do {
    if (Root == State.Node && State.Event == CMARK_EVENT_EXIT)
      break;
    auto Result = parseElement(MC, State);
    Children.push_back(Result.Node);
    State = Result.State;
  } while (!(Root == State.Node && State.Event == CMARK_EVENT_EXIT));
  return State;
}

ParseResult<Text> parseText(MarkupContext &MC, ParseState State) {
  assert(cmark_node_get_type(State.Node) == CMARK_NODE_TEXT
      && State.Event == CMARK_EVENT_ENTER);
  return {Text::create(MC, getLiteralContent(MC, State.Node)), State.next()};
}

ParseResult<BlockQuote> parseBlockQuote(MarkupContext &MC, ParseState State) {
  assert(cmark_node_get_type(State.Node) == CMARK_NODE_BLOCK_QUOTE
      && State.Event == CMARK_EVENT_ENTER);
  SmallVector<MarkupASTNode *, 4> Children;
  auto ResultState = parseChildren(MC, State, Children);
  assert(State.Node == ResultState.Node
      && ResultState.Event == CMARK_EVENT_EXIT);
  return { BlockQuote::create(MC, Children), ResultState.next() };
}

ParseResult<Code> parseCode(MarkupContext &MC, ParseState State) {
  assert(cmark_node_get_type(State.Node) == CMARK_NODE_CODE
      && State.Event == CMARK_EVENT_ENTER);
  return {Code::create(MC, getLiteralContent(MC, State.Node)), State.next()};
}

ParseResult<CodeBlock> parseCodeBlock(MarkupContext &MC, ParseState State) {
  assert(cmark_node_get_type(State.Node) == CMARK_NODE_CODE_BLOCK
      && State.Event == CMARK_EVENT_ENTER);

  StringRef Language("swift");

  if (auto FenceInfo = cmark_node_get_fence_info(State.Node)) {
    StringRef FenceInfoStr(FenceInfo);
    if (!FenceInfoStr.empty())
      Language = MC.allocateCopy(FenceInfoStr);
  }
  return {CodeBlock::create(MC, getLiteralContent(MC, State.Node), Language),
          State.next()};
}

ParseResult<Emphasis> parseEmphasis(MarkupContext &MC, ParseState State) {
  assert(cmark_node_get_type(State.Node) == CMARK_NODE_EMPH
      && State.Event == CMARK_EVENT_ENTER);
  SmallVector<MarkupASTNode *, 2> Children;
  auto ResultState = parseChildren(MC, State, Children);
  assert(State.Node == ResultState.Node
      && ResultState.Event == CMARK_EVENT_EXIT);
  return { Emphasis::create(MC, Children), ResultState.next() };
}

ParseResult<Strong> parseStrong(MarkupContext &MC, ParseState State) {
  assert(cmark_node_get_type(State.Node) == CMARK_NODE_STRONG
      && State.Event == CMARK_EVENT_ENTER);
  SmallVector<MarkupASTNode *, 2> Children;
  auto ResultState = parseChildren(MC, State, Children);
  assert(State.Node == ResultState.Node
      && ResultState.Event == CMARK_EVENT_EXIT);
  return { Strong::create(MC, Children), ResultState.next() };
}

ParseResult<Header> parseHeader(MarkupContext &MC, ParseState State) {
  assert(cmark_node_get_type(State.Node) == CMARK_NODE_HEADING
      && State.Event == CMARK_EVENT_ENTER);
  auto Level = cmark_node_get_header_level(State.Node);
  SmallVector<MarkupASTNode *, 2> Children;
  auto ResultState = parseChildren(MC, State, Children);
  assert(State.Node == ResultState.Node
      && ResultState.Event == CMARK_EVENT_EXIT);
  (void) ResultState;
  return { Header::create(MC, Level, Children), State.next() };
}

ParseResult<HRule> parseHRule(MarkupContext &MC, ParseState State) {
  assert(cmark_node_get_type(State.Node) == CMARK_NODE_THEMATIC_BREAK
      && State.Event == CMARK_EVENT_ENTER);
  return { HRule::create(MC), State.next() };
}

ParseResult<HTML> parseHTML(MarkupContext &MC, ParseState State) {
  assert(cmark_node_get_type(State.Node) == CMARK_NODE_HTML_BLOCK
      && State.Event == CMARK_EVENT_ENTER);
  return {HTML::create(MC, getLiteralContent(MC, State.Node)), State.next()};
}

ParseResult<InlineHTML> parseInlineHTML(MarkupContext &MC, ParseState State) {
  assert(cmark_node_get_type(State.Node) == CMARK_NODE_HTML_INLINE
      && State.Event == CMARK_EVENT_ENTER);
  return {InlineHTML::create(MC, getLiteralContent(MC, State.Node)),
          State.next()};
}

ParseResult<Image> parseImage(MarkupContext &MC, ParseState State) {
  assert(cmark_node_get_type(State.Node) == CMARK_NODE_IMAGE
      && State.Event == CMARK_EVENT_ENTER);
  std::string Destination(cmark_node_get_url(State.Node));
  
  auto NodeTitle = cmark_node_get_title(State.Node);
  std::string TitleString = NodeTitle ? NodeTitle : "";
  auto Title = TitleString.empty() ? std::nullopt
                                   : std::optional<StringRef>(TitleString);

  SmallVector<MarkupASTNode *, 2> Children;
  auto ResultState = parseChildren(MC, State, Children);
  assert(State.Node == ResultState.Node
      && ResultState.Event == CMARK_EVENT_EXIT);
  return { Image::create(MC, Destination, Title, Children), ResultState.next() };
}

ParseResult<Item> parseItem(MarkupContext &MC, ParseState State) {
  assert(cmark_node_get_type(State.Node) == CMARK_NODE_ITEM
      && State.Event == CMARK_EVENT_ENTER);
  SmallVector<MarkupASTNode *, 2> Children;
  auto ResultState = parseChildren(MC, State, Children);
  assert(State.Node == ResultState.Node
      && ResultState.Event == CMARK_EVENT_EXIT);
  return { Item::create(MC, Children), ResultState.next() };
}

ParseResult<LineBreak> parseLineBreak(MarkupContext &MC, ParseState State) {
  assert(cmark_node_get_type(State.Node) == CMARK_NODE_LINEBREAK
      && State.Event == CMARK_EVENT_ENTER);
  return { LineBreak::create(MC), State.next() };
}

ParseResult<SoftBreak> parseSoftBreak(MarkupContext &MC, ParseState State) {
  assert(cmark_node_get_type(State.Node) == CMARK_NODE_SOFTBREAK
      && State.Event == CMARK_EVENT_ENTER);
  return { SoftBreak::create(MC), State.next() };
}

ParseResult<Link> parseLink(MarkupContext &MC, ParseState State) {
  assert(cmark_node_get_type(State.Node) == CMARK_NODE_LINK
      && State.Event == CMARK_EVENT_ENTER);
  std::string Destination(cmark_node_get_url(State.Node));
  SmallVector<MarkupASTNode *, 2> Children;
  auto ResultState = parseChildren(MC, State, Children);
  assert(State.Node == ResultState.Node
      && ResultState.Event == CMARK_EVENT_EXIT);
  return { Link::create(MC, Destination, Children), ResultState.next() };
}

ParseResult<InlineAttributes> parseAttribute(MarkupContext &MC, ParseState State) {
  assert(cmark_node_get_type(State.Node) == CMARK_NODE_ATTRIBUTE && State.Event == CMARK_EVENT_ENTER);
  std::string Attributes(cmark_node_get_attributes(State.Node));
  SmallVector<MarkupASTNode *, 2> Children;
  auto ResultState = parseChildren(MC, State, Children);
  assert(State.Node == ResultState.Node && ResultState.Event == CMARK_EVENT_EXIT);
  return { InlineAttributes::create(MC, Attributes, Children), ResultState.next() };
}

ParseResult<List> parseList(MarkupContext &MC, ParseState State) {
  assert(cmark_node_get_type(State.Node) == CMARK_NODE_LIST
      && State.Event == CMARK_EVENT_ENTER);
  auto ListRoot = State.Node;
  auto IsOrdered = cmark_node_get_list_type(ListRoot) == CMARK_ORDERED_LIST;
  SmallVector<MarkupASTNode *, 3> Children;
  auto ResultState = parseChildren(MC, State, Children);
  assert(State.Node == ResultState.Node
      && ResultState.Event == CMARK_EVENT_EXIT);
  return { List::create(MC, Children, IsOrdered), ResultState.next() };
}

ParseResult<Paragraph> parseParagraph(MarkupContext &MC, ParseState State) {
  assert(cmark_node_get_type(State.Node) == CMARK_NODE_PARAGRAPH
      && State.Event == CMARK_EVENT_ENTER);
  SmallVector<MarkupASTNode *, 3> Children;
  auto ResultState = parseChildren(MC, State, Children);
  assert(State.Node == ResultState.Node
      && ResultState.Event == CMARK_EVENT_EXIT);
  return { Paragraph::create(MC, Children), ResultState.next() };
}

ParseResult<MarkupASTNode> parseElement(MarkupContext &MC, ParseState State) {
  assert(State.Event == CMARK_EVENT_ENTER);
  auto NodeType = cmark_node_get_type(State.Node);
  switch (NodeType) {
  case CMARK_NODE_DOCUMENT: {
    llvm_unreachable("Markup documents cannot be nested");
  }
  case CMARK_NODE_FOOTNOTE_REFERENCE:
  case CMARK_NODE_FOOTNOTE_DEFINITION: {
    llvm_unreachable("Footnotes are not currently parsed by swiftMarkup");
  }
  case CMARK_NODE_ATTRIBUTE: {
    return parseAttribute(MC, State);
  }
  case CMARK_NODE_BLOCK_QUOTE: {
    return parseBlockQuote(MC, State);
  }
  case CMARK_NODE_CODE: {
    return parseCode(MC, State);
  }
  case CMARK_NODE_CODE_BLOCK: {
    return parseCodeBlock(MC, State);
  }
  case CMARK_NODE_EMPH: {
    return parseEmphasis(MC, State);
  }
  case CMARK_NODE_HEADING: {
    return parseHeader(MC, State);
  }
  case CMARK_NODE_HTML_BLOCK: {
    return parseHTML(MC, State);
  }
  case CMARK_NODE_HTML_INLINE: {
    return parseInlineHTML(MC, State);
  }
  case CMARK_NODE_IMAGE: {
    return parseImage(MC, State);
  }
  case CMARK_NODE_ITEM: {
    return parseItem(MC, State);
  }
  case CMARK_NODE_LINEBREAK: {
    return parseLineBreak(MC, State);
  }
  case CMARK_NODE_LINK: {
    return parseLink(MC, State);
  }
  case CMARK_NODE_LIST: {
    return parseList(MC, State);
  }
  case CMARK_NODE_PARAGRAPH: {
    return parseParagraph(MC, State);
  }
  case CMARK_NODE_SOFTBREAK: {
    return parseSoftBreak(MC, State);
  }
  case CMARK_NODE_STRONG: {
    return parseStrong(MC, State);
  }
  case CMARK_NODE_TEXT: {
    return parseText(MC, State);
  }
  case CMARK_NODE_THEMATIC_BREAK: {
    return parseHRule(MC, State);
  }
  default: {
    llvm_unreachable("Can't parse a Markup node of type 'None'");
  }
  }
}

static Document *parseDocumentImpl(MarkupContext &MC, StringRef String) {
  auto CMarkDoc =
      cmark_parse_document(String.data(), String.size(), CMARK_OPT_SMART);

  if (CMarkDoc == nullptr)
    return nullptr;

  ParseState State(cmark_iter_new(CMarkDoc));
  // Prime the parser.
  State = State.next();
  SmallVector<MarkupASTNode *, 8> Children;
  assert(cmark_node_get_type(State.Node) == CMARK_NODE_DOCUMENT
      && State.Event == CMARK_EVENT_ENTER);
  auto ResultState = parseChildren(MC, State, Children);
  assert(State.Node == ResultState.Node
      && ResultState.Event == CMARK_EVENT_EXIT);
  State = ResultState.next();
  assert(State.Event == CMARK_EVENT_DONE);
  cmark_node_free(CMarkDoc);
  cmark_iter_free(State.Iter);
  return Document::create(MC, Children);
}

Document *swift::markup::parseDocument(MarkupContext &MC, LineList &LL) {
  return parseDocumentImpl(MC, LL.str());
}

Document *swift::markup::parseDocument(MarkupContext &MC, StringRef String) {
  return parseDocumentImpl(MC, MC.allocateCopy(String));
}
