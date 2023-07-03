//===--- AST.h - Markup AST nodes -------------------------------*- C++ -*-===//
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
#ifndef SWIFT_MARKUP_AST_H
#define SWIFT_MARKUP_AST_H

#include "swift/Markup/LineList.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/TrailingObjects.h"

namespace swift {
namespace markup {

class MarkupContext;
class MarkupASTNode;
class Paragraph;
class ParamField;
class ReturnsField;
class TagField;
class ThrowsField;
class LocalizationKeyField;

/// The basic structure of a doc comment attached to a Swift
/// declaration.
struct CommentParts {
  llvm::Optional<const Paragraph *> Brief;
  ArrayRef<const MarkupASTNode *> BodyNodes;
  ArrayRef<ParamField *> ParamFields;
  llvm::Optional<const swift::markup::ReturnsField *> ReturnsField;
  llvm::Optional<const swift::markup::ThrowsField *> ThrowsField;
  llvm::SmallSetVector<StringRef, 8> Tags;
  llvm::Optional<const swift::markup::LocalizationKeyField *>
      LocalizationKeyField;

  bool isEmpty() const {
    return !Brief.has_value() &&
           !ReturnsField.has_value() &&
           !ThrowsField.has_value() &&
           BodyNodes.empty() &&
           ParamFields.empty();
  }

  bool hasFunctionDocumentation() const {
    return !ParamFields.empty() ||
             ReturnsField.has_value() ||
             ThrowsField.has_value();
  }
};

#define MARKUP_AST_NODE(Id, Parent) class Id;
#define ABSTRACT_MARKUP_AST_NODE(Id, Parent) class Id;
#define MARKUP_AST_NODE_RANGE(Id, FirstId, LastId)
#include "swift/Markup/ASTNodes.def"

enum class ASTNodeKind : uint8_t {
#define MARKUP_AST_NODE(Id, Parent) Id,
#define ABSTRACT_MARKUP_AST_NODE(Id, Parent)
#define MARKUP_AST_NODE_RANGE(Id, FirstId, LastId) \
  First_##Id = FirstId, Last_##Id = LastId,
#include "swift/Markup/ASTNodes.def"
};

class alignas(void *) MarkupASTNode {
  MarkupASTNode(const MarkupASTNode &) = delete;
  void operator=(const MarkupASTNode &) = delete;

protected:
  ASTNodeKind Kind;

public:

  MarkupASTNode(ASTNodeKind Kind) : Kind(Kind) {}

  ASTNodeKind getKind() const { return Kind; }

  void *operator new(size_t Bytes, MarkupContext &MC,
                     unsigned alignment = alignof(MarkupASTNode));
  void *operator new(size_t Bytes, void *Mem) {
    assert(Mem);
    return Mem;
  }

  ArrayRef<MarkupASTNode *> getChildren();
  ArrayRef<const MarkupASTNode *> getChildren() const;
  void *operator new(size_t Bytes) = delete;
  void operator delete(void *Data) = delete;
};

#pragma mark Markdown Nodes

class Document final : public MarkupASTNode,
    private llvm::TrailingObjects<Document, MarkupASTNode *> {
  friend TrailingObjects;

  size_t NumChildren;

  Document(ArrayRef<MarkupASTNode*> Children);

public:
  static Document *create(MarkupContext &MC,
                          ArrayRef<MarkupASTNode *> Children);

  ArrayRef<MarkupASTNode *> getChildren() {
    return {getTrailingObjects<MarkupASTNode *>(), NumChildren};
  }

  ArrayRef<const MarkupASTNode *> getChildren() const {
    return {getTrailingObjects<MarkupASTNode *>(), NumChildren};
  }

  static bool classof(const MarkupASTNode *N) {
    return N->getKind() == ASTNodeKind::Document;
  }
};

class BlockQuote final : public MarkupASTNode,
    private llvm::TrailingObjects<BlockQuote, MarkupASTNode *> {
  friend TrailingObjects;

  size_t NumChildren;

  BlockQuote(ArrayRef<MarkupASTNode *> Children);

public:
  static BlockQuote *create(MarkupContext &MC, ArrayRef<MarkupASTNode *> Children);

  ArrayRef<MarkupASTNode *> getChildren() {
    return {getTrailingObjects<MarkupASTNode *>(), NumChildren};
  }

  ArrayRef<const MarkupASTNode *> getChildren() const {
    return {getTrailingObjects<MarkupASTNode *>(), NumChildren};
  }

  static bool classof(const MarkupASTNode *N) {
    return N->getKind() == ASTNodeKind::BlockQuote;
  }
};

class List final : public MarkupASTNode,
    private llvm::TrailingObjects<List, MarkupASTNode *> {
  friend TrailingObjects;

  size_t NumChildren;
  bool Ordered;

  List(ArrayRef<MarkupASTNode *> Children, bool IsOrdered);

public:
  static List *create(MarkupContext &MC, ArrayRef<MarkupASTNode *> Items,
                      bool IsOrdered);

  ArrayRef<MarkupASTNode *> getChildren() {
    return {getTrailingObjects<MarkupASTNode *>(), NumChildren};
  }

  ArrayRef<const MarkupASTNode *> getChildren() const {
    return {getTrailingObjects<MarkupASTNode *>(), NumChildren};
  }

  void setChildren(ArrayRef<MarkupASTNode *> NewChildren) {
    assert(NewChildren.size() <= NumChildren);
    std::copy(NewChildren.begin(), NewChildren.end(),
              getTrailingObjects<MarkupASTNode *>());
    NumChildren = NewChildren.size();
  }

  bool isOrdered() const {
    return Ordered;
  }

  static bool classof(const MarkupASTNode *N) {
    return N->getKind() == ASTNodeKind::List;
  }
};

class Item final : public MarkupASTNode,
    private llvm::TrailingObjects<Item, MarkupASTNode *> {
  friend TrailingObjects;

  size_t NumChildren;

  Item(ArrayRef<MarkupASTNode *> Children);

public:
  static Item *create(MarkupContext &MC, ArrayRef<MarkupASTNode *> Children);

  ArrayRef<MarkupASTNode *> getChildren() {
    return {getTrailingObjects<MarkupASTNode *>(), NumChildren};
  }

  ArrayRef<const MarkupASTNode *> getChildren() const {
    return {getTrailingObjects<MarkupASTNode *>(), NumChildren};
  }

  static bool classof(const MarkupASTNode *N) {
    return N->getKind() == ASTNodeKind::Item;
  }
};

class CodeBlock final : public MarkupASTNode {
  StringRef LiteralContent;
  StringRef Language;

  CodeBlock(StringRef LiteralContent, StringRef Language)
      : MarkupASTNode(ASTNodeKind::CodeBlock),
        LiteralContent(LiteralContent),
        Language(Language) {}

public:
  static CodeBlock *create(MarkupContext &MC, StringRef LiteralContent,
                           StringRef Language);

  StringRef getLiteralContent() const { return LiteralContent; };
  StringRef getLanguage() const { return Language; };

  ArrayRef<const MarkupASTNode *> getChildren() const {
    return {};
  }

  ArrayRef<MarkupASTNode *> getChildren() {
    return {};
  }

  static bool classof(const MarkupASTNode *N) {
    return N->getKind() == ASTNodeKind::CodeBlock;
  }
};

class HTML final : public MarkupASTNode {
  StringRef LiteralContent;
  HTML(StringRef LiteralContent)
      : MarkupASTNode(ASTNodeKind::HTML),
        LiteralContent(LiteralContent) {}
public:
  static HTML *create(MarkupContext &MC, StringRef LiteralContent);
  StringRef getLiteralContent() const { return LiteralContent; };

  static bool classof(const MarkupASTNode *N) {
    return N->getKind() == ASTNodeKind::HTML;
  }
};

class Paragraph final : public MarkupASTNode,
    private llvm::TrailingObjects<Paragraph, MarkupASTNode *> {
  friend TrailingObjects;

  size_t NumChildren;

  Paragraph(ArrayRef<MarkupASTNode *> Children);

public:
  static Paragraph *create(MarkupContext &MC,
                           ArrayRef<MarkupASTNode *> Children);

  ArrayRef<MarkupASTNode *> getChildren() {
    return {getTrailingObjects<MarkupASTNode *>(), NumChildren};
  }

  ArrayRef<const MarkupASTNode *> getChildren() const {
    return {getTrailingObjects<MarkupASTNode *>(), NumChildren};
  }

  static bool classof(const MarkupASTNode *N) {
    return N->getKind() == ASTNodeKind::Paragraph;
  }
};

class Header final : public MarkupASTNode,
    private llvm::TrailingObjects<Header, MarkupASTNode *> {
  friend TrailingObjects;

  size_t NumChildren;
  unsigned Level;

  Header(unsigned Level, ArrayRef<MarkupASTNode *> Children);

public:
  static Header *create(MarkupContext &MC, unsigned Level,
                        ArrayRef<MarkupASTNode *> Children);

  ArrayRef<MarkupASTNode *> getChildren() {
    return {getTrailingObjects<MarkupASTNode *>(), NumChildren};
  }

  ArrayRef<const MarkupASTNode *> getChildren() const {
    return {getTrailingObjects<MarkupASTNode *>(), NumChildren};
  }

  unsigned getLevel() const {
    return Level;
  }
  static bool classof(const MarkupASTNode *N) {
    return N->getKind() == ASTNodeKind::Header;
  }
};

class HRule final : public MarkupASTNode {
  HRule() : MarkupASTNode(ASTNodeKind::HRule) {}
public:
  static HRule *create(MarkupContext &MC);

  ArrayRef<const MarkupASTNode *> getChildren() const {
    return {};
  }

  ArrayRef<MarkupASTNode *> getChildren() {
    return {};
  }

  static bool classof(const MarkupASTNode *N) {
    return N->getKind() == ASTNodeKind::HRule;
  }
};

class InlineContent : public MarkupASTNode {
public:
  InlineContent(ASTNodeKind Kind) : MarkupASTNode(Kind) {}

  ArrayRef<const MarkupASTNode *> getChildren() const {
    return {};
  }

  ArrayRef<MarkupASTNode *> getChildren() {
    return {};
  }

  static bool classof(const MarkupASTNode *N) {
    return N->getKind() >= ASTNodeKind::First_Inline &&
           N->getKind() <= ASTNodeKind::Last_Inline;
  }
};

class Text final : public InlineContent {
  StringRef LiteralContent;
  Text(StringRef LiteralContent)
    : InlineContent(ASTNodeKind::Text),
      LiteralContent(LiteralContent) {}
public:
  static Text *create(MarkupContext &MC, StringRef LiteralContent);
  StringRef getLiteralContent() const { return LiteralContent; };
  void setLiteralContent(StringRef LC) {
    LiteralContent = LC;
  }

  ArrayRef<const MarkupASTNode *> getChildren() const {
    return {};
  }

  ArrayRef<MarkupASTNode *> getChildren() {
    return {};
  }

  StringRef str() const {
    return LiteralContent;
  }

  static bool classof(const MarkupASTNode *N) {
    return N->getKind() == ASTNodeKind::Text;
  }
};

class SoftBreak final : public InlineContent {
  SoftBreak() : InlineContent(ASTNodeKind::SoftBreak) {}
public:
  static SoftBreak *create(MarkupContext &MC);

  ArrayRef<const MarkupASTNode *> getChildren() const {
    return {};
  }

  ArrayRef<MarkupASTNode *> getChildren() {
    return {};
  }

  StringRef str() const {
    return "\n";
  }

  static bool classof(const MarkupASTNode *N) {
    return N->getKind() == ASTNodeKind::SoftBreak;
  }
};

class LineBreak final : public InlineContent {
  LineBreak() : InlineContent(ASTNodeKind::LineBreak) {}
public:
  static LineBreak *create(MarkupContext &MC);

  ArrayRef<const MarkupASTNode *> getChildren() const {
    return {};
  }

  ArrayRef<MarkupASTNode *> getChildren() {
    return {};
  }

  StringRef str() const {
    return "\n";
  }

  static bool classof(const MarkupASTNode *N) {
    return N->getKind() == ASTNodeKind::LineBreak;
  }
};

class Code final : public InlineContent {
  StringRef LiteralContent;

  Code(StringRef LiteralContent)
      : InlineContent(ASTNodeKind::Code),
        LiteralContent(LiteralContent) {}
public:
  static Code *create(MarkupContext &MC, StringRef LiteralContent);

  StringRef getLiteralContent() const { return LiteralContent; };

  ArrayRef<const MarkupASTNode *> getChildren() const {
    return {};
  }

  ArrayRef<MarkupASTNode *> getChildren() {
    return {};
  }

  StringRef str() const {
    return LiteralContent;
  }

  static bool classof(const MarkupASTNode *N) {
    return N->getKind() == ASTNodeKind::Code;
  }
};

class InlineHTML final : public InlineContent {
  StringRef LiteralContent;
  InlineHTML(StringRef LiteralContent)
    : InlineContent(ASTNodeKind::InlineHTML),
      LiteralContent(LiteralContent) {}
public:
  static InlineHTML *create(MarkupContext &MC, StringRef LiteralContent);

  StringRef getLiteralContent() const { return LiteralContent; };

  ArrayRef<const MarkupASTNode *> getChildren() const {
    return {};
  }

  ArrayRef<MarkupASTNode *> getChildren() {
    return {};
  }

  StringRef str() const {
    return LiteralContent;
  }

  static bool classof(const MarkupASTNode *N) {
    return N->getKind() == ASTNodeKind::InlineHTML;
  }
};

class Emphasis final : public InlineContent,
    private llvm::TrailingObjects<Emphasis, MarkupASTNode *> {
  friend TrailingObjects;

  size_t NumChildren;

  Emphasis(ArrayRef<MarkupASTNode *> Children);
public:
  static Emphasis *create(MarkupContext &MC,
                          ArrayRef<MarkupASTNode *> Children);

  ArrayRef<MarkupASTNode *> getChildren() {
    return {getTrailingObjects<MarkupASTNode *>(), NumChildren};
  }

  ArrayRef<const MarkupASTNode *> getChildren() const {
    return {getTrailingObjects<MarkupASTNode *>(), NumChildren};
  }

  static bool classof(const MarkupASTNode *N) {
    return N->getKind() == ASTNodeKind::Emphasis;
  }
};

class Strong final : public InlineContent,
    private llvm::TrailingObjects<Strong, MarkupASTNode *> {
  friend TrailingObjects;

  size_t NumChildren;

  Strong(ArrayRef<MarkupASTNode *> Children);
public:
  static Strong *create(MarkupContext &MC,
                        ArrayRef<MarkupASTNode *> Children);

  ArrayRef<MarkupASTNode *> getChildren() {
    return {getTrailingObjects<MarkupASTNode *>(), NumChildren};
  }

  ArrayRef<const MarkupASTNode *> getChildren() const {
    return {getTrailingObjects<MarkupASTNode *>(), NumChildren};
  }

  static bool classof(const MarkupASTNode *N) {
    return N->getKind() == ASTNodeKind::Strong;
  }
};

class Link final : public InlineContent,
    private llvm::TrailingObjects<Link, MarkupASTNode *> {
  friend TrailingObjects;

  size_t NumChildren;

  StringRef Destination;

  Link(StringRef Destination, ArrayRef<MarkupASTNode *> Children);

public:
  static Link *create(MarkupContext &MC,
                      StringRef Destination,
                      ArrayRef<MarkupASTNode *> Children);

  StringRef getDestination() const { return Destination; }

  ArrayRef<MarkupASTNode *> getChildren() {
    return {getTrailingObjects<MarkupASTNode *>(), NumChildren};
  }

  ArrayRef<const MarkupASTNode *> getChildren() const {
    return {getTrailingObjects<MarkupASTNode *>(), NumChildren};
  }

  static bool classof(const MarkupASTNode *N) {
    return N->getKind() == ASTNodeKind::Link;
  }
};

class Image final : public InlineContent,
    private llvm::TrailingObjects<Image, MarkupASTNode *> {
  friend TrailingObjects;

  size_t NumChildren;

  // FIXME: Hyperlink destinations can't be wrapped - use a Line
  StringRef Destination;
  llvm::Optional<StringRef> Title;

  Image(StringRef Destination, llvm::Optional<StringRef> Title,
        ArrayRef<MarkupASTNode *> Children);

public:
  static Image *create(MarkupContext &MC, StringRef Destination,
                       llvm::Optional<StringRef> Title,
                       ArrayRef<MarkupASTNode *> Children);

  StringRef getDestination() const { return Destination; }

  bool hasTitle() const {
    return Title.has_value();
  }

  StringRef getTitle() const {
    return StringRef(Title.value());
  }

  ArrayRef<MarkupASTNode *> getChildren() {
    return {getTrailingObjects<MarkupASTNode *>(), NumChildren};
  }

  ArrayRef<const MarkupASTNode *> getChildren() const {
    return {getTrailingObjects<MarkupASTNode *>(), NumChildren};
  }

  static bool classof(const MarkupASTNode *N) {
    return N->getKind() == ASTNodeKind::Image;
  }
};

class InlineAttributes final : public InlineContent, private llvm::TrailingObjects<Image, MarkupASTNode *> {
  friend TrailingObjects;

  // Note that inline attributes are like links, in that there are child inline nodes that are
  // collectively styled by the attribute text. The child nodes are the text that should be
  // displayed.

  size_t NumChildren;
  StringRef Attributes;

  InlineAttributes(StringRef Attributes, ArrayRef<MarkupASTNode *> Children);

public:
  static InlineAttributes *create(MarkupContext &MC, StringRef Attributes, ArrayRef<MarkupASTNode *> Children);

  StringRef getAttributes() const { return Attributes; }

  ArrayRef<MarkupASTNode *> getChildren() {
    return {getTrailingObjects<MarkupASTNode *>(), NumChildren};
  }

  ArrayRef<const MarkupASTNode *> getChildren() const {
    return {getTrailingObjects<MarkupASTNode *>(), NumChildren};
  }

  static bool classof(const MarkupASTNode *N) {
    return N->getKind() == ASTNodeKind::InlineAttributes;
  }
};

#pragma mark Private Extensions

class PrivateExtension : public MarkupASTNode {
protected:
  PrivateExtension(ASTNodeKind Kind)
      : MarkupASTNode(Kind) {}
public:

  ArrayRef<const MarkupASTNode *> getChildren() const {
    return {};
  }

  ArrayRef<MarkupASTNode *> getChildren() {
    return {};
  }

  static bool classof(const MarkupASTNode *N) {
    return N->getKind() >= ASTNodeKind::First_Private &&
      N->getKind() <= ASTNodeKind::Last_Private;
  }
};

class ParamField final : public PrivateExtension,
    private llvm::TrailingObjects<ParamField, MarkupASTNode *> {
  friend TrailingObjects;

  size_t NumChildren;

  StringRef Name;

  // Parameter fields can contain a substructure describing a
  // function or closure parameter.
  llvm::Optional<CommentParts> Parts;

  ParamField(StringRef Name, ArrayRef<MarkupASTNode *> Children);

public:

  static ParamField *create(MarkupContext &MC, StringRef Name,
                            ArrayRef<MarkupASTNode *> Children);

  StringRef getName() const {
    return Name;
  }

  llvm::Optional<CommentParts> getParts() const {
    return Parts;
  }

  void setParts(CommentParts P) {
    Parts = P;
  }

  bool isClosureParameter() const {
    if (!Parts.has_value())
      return false;

    return Parts.value().hasFunctionDocumentation();
  }

  ArrayRef<MarkupASTNode *> getChildren() {
    return {getTrailingObjects<MarkupASTNode *>(), NumChildren};
  }

  ArrayRef<const MarkupASTNode *> getChildren() const {
    return {getTrailingObjects<MarkupASTNode *>(), NumChildren};
  }

  static bool classof(const MarkupASTNode *N) {
    return N->getKind() == ASTNodeKind::ParamField;
  }
};

#define MARKUP_SIMPLE_FIELD(Id, Keyword, XMLKind) \
class Id final : public PrivateExtension, \
    private llvm::TrailingObjects<Id, MarkupASTNode *> { \
  friend TrailingObjects; \
\
  size_t NumChildren; \
\
  Id(ArrayRef<MarkupASTNode *> Children);\
\
public: \
  static Id *create(MarkupContext &MC, ArrayRef<MarkupASTNode *> Children); \
\
  ArrayRef<MarkupASTNode *> getChildren() { \
    return {getTrailingObjects<MarkupASTNode *>(), NumChildren}; \
  } \
\
  ArrayRef<const MarkupASTNode *> getChildren() const { \
    return {getTrailingObjects<MarkupASTNode *>(), NumChildren}; \
  } \
\
  static bool classof(const MarkupASTNode *N) { \
    return N->getKind() == ASTNodeKind::Id; \
  } \
};
#include "swift/Markup/SimpleFields.def"

class MarkupASTWalker {
public:
  void walk(const MarkupASTNode *Node) {
    enter(Node);

    switch(Node->getKind()) {
#define MARKUP_AST_NODE(Id, Parent)                                         \
    case ASTNodeKind::Id:                                                   \
      visit##Id(static_cast<const Id*>(Node));                              \
      break;
#define ABSTRACT_MARKUP_AST_NODE(Id, Parent)
#define MARKUP_AST_NODE_RANGE(Id, FirstId, LastId)
#include "swift/Markup/ASTNodes.def"
    }

    if (shouldVisitChildrenOf(Node))
      for (auto Child : Node->getChildren())
        walk(Child);

    exit(Node);
  }

  virtual bool shouldVisitChildrenOf(const MarkupASTNode *Node) {
    return true;
  }

  virtual void enter(const MarkupASTNode *Node) {}
  virtual void exit(const MarkupASTNode *Node) {}

#define MARKUP_AST_NODE(Id, Parent) \
  virtual void visit##Id(const Id *Node) {}
#define ABSTRACT_MARKUP_AST_NODE(Id, Parent)
#define MARKUP_AST_NODE_RANGE(Id, FirstId, LastId)
#include "swift/Markup/ASTNodes.def"

  virtual ~MarkupASTWalker() = default;
};

MarkupASTNode *createSimpleField(MarkupContext &MC, StringRef Tag,
                                 ArrayRef<MarkupASTNode *> Children);

bool isAFieldTag(StringRef Tag);

void dump(const MarkupASTNode *Node, llvm::raw_ostream &OS, unsigned indent = 0);
void printInlinesUnder(const MarkupASTNode *Node, llvm::raw_ostream &OS,
                       bool PrintDecorators = false);


template <typename ImplClass, typename RetTy = void, typename... Args>
class MarkupASTVisitor {
public:
  RetTy visit(const MarkupASTNode *Node, Args... args) {
    switch (Node->getKind()) {
#define MARKUP_AST_NODE(Id, Parent) \
    case ASTNodeKind::Id: \
      return static_cast<ImplClass*>(this) \
        ->visit##Id(cast<const Id>(Node), \
                    ::std::forward<Args>(args)...);
#define ABSTRACT_MARKUP_AST_NODE(Id, Parent)
#define MARKUP_AST_NODE_RANGE(Id, FirstId, LastId)
#include "swift/Markup/ASTNodes.def"
    }
  }

  virtual ~MarkupASTVisitor() {}
};

} // namespace markup
} // namespace swift

#endif // SWIFT_MARKUP_AST_H
