//===--- AST.cpp - Extraction of raw comments -----------------------------===//
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
///
/// \file
/// This file implements Swift markup AST nodes.
///
//===----------------------------------------------------------------------===//

#include "swift/Markup/AST.h"
#include "swift/Markup/Markup.h"
#include "llvm/ADT/None.h"
#include "llvm/ADT/Optional.h"

using namespace swift;
using namespace markup;

Document::Document(ArrayRef<MarkupASTNode*> Children)
    : MarkupASTNode(ASTNodeKind::Document), NumChildren(Children.size()) {
  std::uninitialized_copy(Children.begin(), Children.end(),
                          getTrailingObjects<MarkupASTNode *>());
}

Document *Document::create(MarkupContext &MC,
                           ArrayRef<swift::markup::MarkupASTNode *> Children) {
  void *Mem = MC.allocate(totalSizeToAlloc<MarkupASTNode *>(Children.size()),
                          alignof(Document));
  return new (Mem) Document(Children);
}

BlockQuote::BlockQuote(ArrayRef<MarkupASTNode*> Children)
    : MarkupASTNode(ASTNodeKind::BlockQuote), NumChildren(Children.size()) {
  std::uninitialized_copy(Children.begin(), Children.end(),
                          getTrailingObjects<MarkupASTNode *>());
}

BlockQuote *BlockQuote::create(MarkupContext &MC, ArrayRef<MarkupASTNode *> Children) {
  void *Mem = MC.allocate(totalSizeToAlloc<MarkupASTNode *>(Children.size()),
                          alignof(BlockQuote));
  return new (Mem) BlockQuote(Children);
}

HTML *HTML::create(MarkupContext &MC, StringRef LiteralContent) {
  void *Mem = MC.allocate(sizeof(HTML), alignof(HTML));
  return new (Mem) HTML(LiteralContent);
}

InlineHTML *InlineHTML::create(MarkupContext &MC, StringRef LiteralContent) {
  void *Mem = MC.allocate(sizeof(InlineHTML), alignof(InlineHTML));
  return new (Mem) InlineHTML(LiteralContent);
}

Code *Code::create(MarkupContext &MC, StringRef LiteralContent) {
  void *Mem = MC.allocate(sizeof(Code), alignof(Code));
  return new (Mem) Code(LiteralContent);
}

CodeBlock *CodeBlock::create(MarkupContext &MC, StringRef LiteralContent,
                             StringRef Language) {
  void *Mem = MC.allocate(sizeof(CodeBlock), alignof(CodeBlock));
  return new (Mem) CodeBlock(LiteralContent, Language);
}

List::List(ArrayRef<MarkupASTNode *> Children, bool IsOrdered)
    : MarkupASTNode(ASTNodeKind::List), NumChildren(Children.size()),
      Ordered(IsOrdered) {
  std::uninitialized_copy(Children.begin(), Children.end(),
                          getTrailingObjects<MarkupASTNode *>());
}

List *List::create(MarkupContext &MC, ArrayRef<MarkupASTNode *> Children,
                   bool IsOrdered) {
  void *Mem = MC.allocate(totalSizeToAlloc<MarkupASTNode *>(Children.size()),
                          alignof(List));
  return new (Mem) List(Children, IsOrdered);
}

Item::Item(ArrayRef<MarkupASTNode*> Children)
    : MarkupASTNode(ASTNodeKind::Item), NumChildren(Children.size()) {
  std::uninitialized_copy(Children.begin(), Children.end(),
                          getTrailingObjects<MarkupASTNode *>());
}

Item *Item::create(MarkupContext &MC, ArrayRef<MarkupASTNode *> Children) {
  void *Mem = MC.allocate(totalSizeToAlloc<MarkupASTNode *>(Children.size()),
                          alignof(Item));
  return new (Mem) Item(Children);
}

Link::Link(StringRef Destination, ArrayRef<MarkupASTNode *> Children)
    : InlineContent(ASTNodeKind::Link), NumChildren(Children.size()),
      Destination(Destination) {
  std::uninitialized_copy(Children.begin(), Children.end(),
                          getTrailingObjects<MarkupASTNode *>());
}

Link *Link::create(MarkupContext &MC, StringRef Destination,
                   ArrayRef<MarkupASTNode *> Children) {
  void *Mem = MC.allocate(totalSizeToAlloc<MarkupASTNode *>(Children.size()),
                          alignof(Link));
  StringRef DestinationCopy = MC.allocateCopy(Destination);
  return new (Mem) Link(DestinationCopy, Children);
}

Image::Image(StringRef Destination, llvm::Optional<StringRef> Title,
             ArrayRef<MarkupASTNode *> Children)
    : InlineContent(ASTNodeKind::Image), NumChildren(Children.size()),
      Destination(Destination), Title(Title) {
  std::uninitialized_copy(Children.begin(), Children.end(),
                          getTrailingObjects<MarkupASTNode *>());
}

Image *Image::create(MarkupContext &MC, StringRef Destination,
                     llvm::Optional<StringRef> Title,
                     ArrayRef<MarkupASTNode *> Children) {
  void *Mem = MC.allocate(totalSizeToAlloc<MarkupASTNode *>(Children.size()),
                          alignof(Image));
  StringRef DestinationCopy = MC.allocateCopy(Destination);
  llvm::Optional<StringRef> TitleCopy;
  if (Title)
    TitleCopy = MC.allocateCopy(*Title);
  return new (Mem) Image(DestinationCopy, TitleCopy, Children);
}

Header::Header(unsigned Level, ArrayRef<MarkupASTNode *> Children)
    : MarkupASTNode(ASTNodeKind::Header), NumChildren(Children.size()),
      Level(Level) {
  std::uninitialized_copy(Children.begin(), Children.end(),
                          getTrailingObjects<MarkupASTNode *>());
}

Header *Header::create(MarkupContext &MC, unsigned Level,
                       ArrayRef<MarkupASTNode *> Children) {
  void *Mem = MC.allocate(totalSizeToAlloc<MarkupASTNode *>(Children.size()),
                          alignof(Header));
  return new (Mem) Header(Level, Children);
}

Paragraph::Paragraph(ArrayRef<MarkupASTNode *> Children)
    : MarkupASTNode(ASTNodeKind::Paragraph),
      NumChildren(Children.size()) {
  std::uninitialized_copy(Children.begin(), Children.end(),
                          getTrailingObjects<MarkupASTNode *>());
}

Paragraph *Paragraph::create(MarkupContext &MC,
                             ArrayRef<swift::markup::MarkupASTNode *> Children) {
  void *Mem = MC.allocate(totalSizeToAlloc<MarkupASTNode *>(Children.size()),
                          alignof(Paragraph));
  return new (Mem) Paragraph(Children);
}

InlineAttributes::InlineAttributes(StringRef Attributes, ArrayRef<MarkupASTNode *> Children) : InlineContent(ASTNodeKind::InlineAttributes), NumChildren(Children.size()), Attributes(Attributes) {
  std::uninitialized_copy(Children.begin(), Children.end(), getTrailingObjects<MarkupASTNode *>());
}

InlineAttributes *InlineAttributes::create(MarkupContext &MC, StringRef Attributes, ArrayRef<MarkupASTNode *> Children) {
  void *Mem = MC.allocate(totalSizeToAlloc<MarkupASTNode *>(Children.size()), alignof(InlineAttributes));
  StringRef AttrsCopy = MC.allocateCopy(Attributes);
  return new (Mem) InlineAttributes(AttrsCopy, Children);
}

HRule *HRule::create(MarkupContext &MC) {
  void *Mem = MC.allocate(sizeof(HRule), alignof(HRule));
  return new (Mem) HRule();
}

Text *Text::create(MarkupContext &MC, StringRef LiteralContent) {
  void *Mem = MC.allocate(sizeof(Text), alignof(Text));
  return new (Mem) Text(LiteralContent);
}

SoftBreak *SoftBreak::create(MarkupContext &MC) {
  void *Mem = MC.allocate(sizeof(SoftBreak), alignof(SoftBreak));
  return new (Mem) SoftBreak();
}

LineBreak *LineBreak::create(MarkupContext &MC) {
  void *Mem = MC.allocate(sizeof(LineBreak), alignof(LineBreak));
  return new (Mem) LineBreak();
}

Emphasis::Emphasis(ArrayRef<MarkupASTNode *> Children)
    : InlineContent(ASTNodeKind::Emphasis), NumChildren(Children.size()) {
  std::uninitialized_copy(Children.begin(), Children.end(),
                          getTrailingObjects<MarkupASTNode *>());
}

Emphasis *Emphasis::create(MarkupContext &MC,
                           ArrayRef<swift::markup::MarkupASTNode *> Children) {
  void *Mem = MC.allocate(totalSizeToAlloc<MarkupASTNode *>(Children.size()),
                          alignof(Emphasis));
  return new (Mem) Emphasis(Children);
}

Strong::Strong(ArrayRef<MarkupASTNode *> Children)
    : InlineContent(ASTNodeKind::Strong), NumChildren(Children.size()) {
  std::uninitialized_copy(Children.begin(), Children.end(),
                          getTrailingObjects<MarkupASTNode *>());
}

Strong *Strong::create(MarkupContext &MC,
                       ArrayRef<swift::markup::MarkupASTNode *> Children) {
  void *Mem = MC.allocate(totalSizeToAlloc<MarkupASTNode *>(Children.size()),
                          alignof(Strong));
  return new (Mem) Strong(Children);
}

ParamField::ParamField(StringRef Name, ArrayRef<MarkupASTNode *> Children)
    : PrivateExtension(ASTNodeKind::ParamField), NumChildren(Children.size()),
      Name(Name), Parts(llvm::None) {
  std::uninitialized_copy(Children.begin(), Children.end(),
                          getTrailingObjects<MarkupASTNode *>());
}

ParamField *ParamField::create(MarkupContext &MC, StringRef Name,
                               ArrayRef<MarkupASTNode *> Children) {
  void *Mem = MC.allocate(totalSizeToAlloc<MarkupASTNode *>(Children.size()),
                          alignof(ParamField));
  return new (Mem) ParamField(Name, Children);
}

#define MARKUP_SIMPLE_FIELD(Id, Keyword, XMLKind) \
Id *Id::create(MarkupContext &MC, ArrayRef<MarkupASTNode *> Children) { \
  void *Mem = MC.allocate(totalSizeToAlloc<MarkupASTNode *>(Children.size()), \
                          alignof(Id)); \
  return new (Mem) Id(Children); \
} \
\
Id::Id(ArrayRef<MarkupASTNode *> Children) \
    : PrivateExtension(ASTNodeKind::Id), NumChildren(Children.size()) { \
  std::uninitialized_copy(Children.begin(), Children.end(), \
                          getTrailingObjects<MarkupASTNode *>()); \
}
#include "swift/Markup/SimpleFields.def"

ArrayRef<MarkupASTNode *> MarkupASTNode::getChildren() {
  switch (Kind) {
#define MARKUP_AST_NODE(Id, Parent) \
  case ASTNodeKind::Id: \
      return cast<Id>(this)->getChildren();
#define ABSTRACT_MARKUP_AST_NODE(Id, Parent) class Id;
#define MARKUP_AST_NODE_RANGE(Id, FirstId, LastId)
#include "swift/Markup/ASTNodes.def"
  }

  llvm_unreachable("Unhandled ASTNodeKind in switch.");
}

ArrayRef<const MarkupASTNode *> MarkupASTNode::getChildren() const {
  switch (Kind) {
#define MARKUP_AST_NODE(Id, Parent) \
case ASTNodeKind::Id: \
return cast<Id>(this)->getChildren();
#define ABSTRACT_MARKUP_AST_NODE(Id, Parent) class Id;
#define MARKUP_AST_NODE_RANGE(Id, FirstId, LastId)
#include "swift/Markup/ASTNodes.def"
  }

  llvm_unreachable("Unhandled ASTNodeKind in switch.");
}

void swift::markup::printInlinesUnder(const MarkupASTNode *Node,
                                     llvm::raw_ostream &OS,
                                     bool PrintDecorators) {
  auto printChildren = [](const ArrayRef<const MarkupASTNode *> Children,
                          llvm::raw_ostream &OS) {
    for (auto Child = Children.begin(); Child != Children.end(); ++Child)
      swift::markup::printInlinesUnder(*Child, OS);
  };

  switch (Node->getKind()) {
  case swift::markup::ASTNodeKind::HTML: {
    auto H = cast<HTML>(Node);
    OS << H->getLiteralContent();
    break;
  }
  case swift::markup::ASTNodeKind::InlineHTML: {
    auto IH = cast<InlineHTML>(Node);
    OS << IH->getLiteralContent();
    break;
  }
  case swift::markup::ASTNodeKind::HRule:
    OS << '\n';
    break;
  case swift::markup::ASTNodeKind::Text: {
    auto T = cast<Text>(Node);
    OS << T->getLiteralContent();
    break;
  }
  case swift::markup::ASTNodeKind::SoftBreak:
    OS << ' ';
    break;
  case swift::markup::ASTNodeKind::LineBreak:
    OS << '\n';
    break;
  case swift::markup::ASTNodeKind::Code: {
    auto C = cast<Code>(Node);
    if (PrintDecorators)
      OS << '`';

    OS << C->getLiteralContent();

    if (PrintDecorators)
      OS << '`';

    break;
  }
  case swift::markup::ASTNodeKind::CodeBlock: {
    auto CB = cast<CodeBlock>(Node);
    if (PrintDecorators) OS << "``";

    OS << CB->getLiteralContent();

    if (PrintDecorators) OS << "``";

    break;
  }
  case swift::markup::ASTNodeKind::Emphasis: {
    auto E = cast<Emphasis>(Node);
    if (PrintDecorators) OS << '*';
    printChildren(E->getChildren(), OS);
    if (PrintDecorators) OS << '*';
    break;
  }
  case swift::markup::ASTNodeKind::Strong: {
    auto S = cast<Strong>(Node);
    if (PrintDecorators) OS << "**";
    printChildren(S->getChildren(), OS);
    if (PrintDecorators) OS << "**";
    break;
  }
  default:
    printChildren(Node->getChildren(), OS);
  }
  OS.flush();
}

swift::markup::MarkupASTNode *swift::markup::createSimpleField(
    MarkupContext &MC,
    StringRef Tag,
    ArrayRef<swift::markup::MarkupASTNode *> Children) {
  if (false) {

  }
#define MARKUP_SIMPLE_FIELD(Id, Keyword, XMLKind)                              \
  else if (Tag.compare_insensitive(#Keyword) == 0) {                           \
    return Id::create(MC, Children);                                           \
  }
#include "swift/Markup/SimpleFields.def"
  llvm_unreachable("Given tag not for any simple markup field");
}

bool swift::markup::isAFieldTag(StringRef Tag) {
  if (false) {

  }
#define MARKUP_SIMPLE_FIELD(Id, Keyword, XMLKind)                              \
  else if (Tag.compare_insensitive(#Keyword) == 0) {                           \
    return true;                                                               \
  }
#include "swift/Markup/SimpleFields.def"
  return false;
}

void swift::markup::dump(const MarkupASTNode *Node, llvm::raw_ostream &OS,
                        unsigned indent) {
  auto dumpChildren = [](const ArrayRef<const MarkupASTNode *> Children,
                         llvm::raw_ostream &OS, unsigned indent) {
    OS << '\n';
    for (auto Child = Children.begin(); Child != Children.end(); ++Child) {
      swift::markup::dump(*Child, OS, indent + 1);
      if (Child != Children.end() - 1)
        OS << '\n';
    }
  };

  auto simpleEscapingPrint = [](StringRef LiteralContent,
                                llvm::raw_ostream &OS) {
    OS << "\"";
    for (auto C : LiteralContent) {
      switch (C) {
      case '\n':
        OS << "\\n";
        break;
      case '\r':
        OS << "\\r";
        break;
      case '\t':
        OS << "\\t";
        break;
      case '"':
        OS << "\\\"";
        break;
      default:
        OS << C;
      }
    }
    OS << "\"";
  };

  for (unsigned i = 0; i < indent; ++i) {
    OS << ' ';
  }

  OS << '(';
  switch (Node->getKind()) {
  case swift::markup::ASTNodeKind::Document: {
    OS << "Document: Children=" << Node->getChildren().size();
    dumpChildren(Node->getChildren(), OS, indent + 1);
    break;
  }
  case swift::markup::ASTNodeKind::InlineAttributes: {
    auto A = cast<InlineAttributes>(Node);
    OS << "Attribute:";
    OS << " Attributes=" << A->getAttributes();
    OS << " Children=" << Node->getChildren().size();
    dumpChildren(Node->getChildren(), OS, indent + 1);
    break;
  }
  case swift::markup::ASTNodeKind::BlockQuote: {
    OS << "BlockQuote: Children=" << Node->getChildren().size();
    dumpChildren(Node->getChildren(), OS, indent + 1);
    break;
  }
  case swift::markup::ASTNodeKind::List: {
    auto L = cast<List>(Node);
    OS << "List: " << (L->isOrdered() ? "Ordered " : "Unordered ");
    OS << "Items=" << Node->getChildren().size();
    dumpChildren(Node->getChildren(), OS, indent + 1);
    break;
  }
  case swift::markup::ASTNodeKind::Item: {
    OS << "Item: Children=" << Node->getChildren().size();
    dumpChildren(Node->getChildren(), OS, indent + 1);
    break;
  }
  case swift::markup::ASTNodeKind::HTML: {
    auto H = cast<HTML>(Node);
    OS << "HTML: Content=";
    simpleEscapingPrint(H->getLiteralContent(), OS);
    break;
  }
  case swift::markup::ASTNodeKind::InlineHTML: {
    auto IH = cast<InlineHTML>(Node);
    OS << "InlineHTML: Content=";
    simpleEscapingPrint(IH->getLiteralContent(), OS);
    break;
  }
  case swift::markup::ASTNodeKind::Paragraph: {
    OS << "Paragraph: Children=" << Node->getChildren().size();
    dumpChildren(Node->getChildren(), OS, indent + 1);
    break;
  }
  case swift::markup::ASTNodeKind::Header: {
    auto H = cast<Header>(Node);
    OS << "Header: Level=" << H->getLevel();
    dumpChildren(Node->getChildren(), OS, indent + 1);
    break;
  }
  case swift::markup::ASTNodeKind::HRule: {
    OS << "HRule";
    break;
  }
  case swift::markup::ASTNodeKind::Text: {
    auto T = cast<Text>(Node);
    OS << "Text: Content=";
    simpleEscapingPrint(T->getLiteralContent(), OS);
    break;
  }
  case swift::markup::ASTNodeKind::SoftBreak: {
    OS << "SoftBreak";
    break;
  }
  case swift::markup::ASTNodeKind::LineBreak: {
    OS << "LineBreak";
    break;
  }
  case swift::markup::ASTNodeKind::CodeBlock: {
    auto CB = cast<CodeBlock>(Node);
    OS << "CodeBlock: ";
    OS << "Language=";
    simpleEscapingPrint(CB->getLanguage(), OS);
    OS << " Content=";
    simpleEscapingPrint(CB->getLiteralContent(), OS);
    break;
  }
  case swift::markup::ASTNodeKind::Code: {
    auto C = cast<Code>(Node);
    OS << "Code: Content=\"";
    simpleEscapingPrint(C->getLiteralContent(), OS);
    OS << "\"";
    break;
  }
  case swift::markup::ASTNodeKind::Strong: {
    OS << "Strong: Children=" << Node->getChildren().size();
    dumpChildren(Node->getChildren(), OS, indent + 1);
    break;
  }
  case swift::markup::ASTNodeKind::Emphasis: {
    OS << "Emphasis: Children=" << Node->getChildren().size();
    dumpChildren(Node->getChildren(), OS, indent + 1);
    break;
  }
  case swift::markup::ASTNodeKind::Link: {
    auto L = cast<Link>(Node);
    OS << "Link: Destination=";
    simpleEscapingPrint(L->getDestination(), OS);
    OS << ' ' << "Children=" << L->getChildren().size();
    dumpChildren(Node->getChildren(), OS, indent + 1);
    break;
  }
  case swift::markup::ASTNodeKind::Image: {
    auto I = cast<Image>(Node);
    OS << "Image: Destination=";
    simpleEscapingPrint(I->getDestination(), OS);
    OS << ' ' << "Children=" << I->getChildren().size();
    dumpChildren(Node->getChildren(), OS, indent + 1);
    break;
  }
  case swift::markup::ASTNodeKind::ParamField: {
    auto PF = cast<ParamField>(Node);
    OS << "ParamField: Name=";
    simpleEscapingPrint(PF->getName(), OS);
    OS << " Children=" << PF->getChildren().size();
    dumpChildren(Node->getChildren(), OS, indent + 1);
    break;
  }

#define MARKUP_SIMPLE_FIELD(Id, Keyword, XMLKind) \
  case swift::markup::ASTNodeKind::Id: { \
    auto Field = cast<Id>(Node); \
    OS << #Id << ": Children=" << Field->getChildren().size(); \
    dumpChildren(Node->getChildren(), OS, indent + 1); \
    break; \
  }
#include "swift/Markup/SimpleFields.def"

  default:
    llvm_unreachable("Can't dump Markup AST Node: unknown node kind");
  }
  OS << ')';
}
