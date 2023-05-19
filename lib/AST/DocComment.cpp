//===--- DocComment.cpp - Extraction of doc comments ----------------------===//
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
/// This file implements extraction of documentation comments from a Swift
/// Markup AST tree.
///
//===----------------------------------------------------------------------===//

#include "swift/AST/ASTContext.h"
#include "swift/AST/Comment.h"
#include "swift/AST/Decl.h"
#include "swift/AST/FileUnit.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/AST/Types.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/RawComment.h"
#include "swift/Markup/Markup.h"
#include <queue>

using namespace swift;

void *DocComment::operator new(size_t Bytes, swift::markup::MarkupContext &MC,
                               unsigned Alignment) {
  return MC.allocate(Bytes, Alignment);
}

namespace {
Optional<swift::markup::ParamField *> extractParamOutlineItem(
    swift::markup::MarkupContext &MC,
    swift::markup::MarkupASTNode *Node) {

  auto Item = dyn_cast<swift::markup::Item>(Node);
  if (!Item)
    return None;

  auto Children = Item->getChildren();
  if (Children.empty())
    return None;

  auto FirstChild = Children.front();
  auto FirstParagraph = dyn_cast<swift::markup::Paragraph>(FirstChild);
  if (!FirstParagraph)
    return None;

  auto FirstParagraphChildren = FirstParagraph->getChildren();
  if (FirstParagraphChildren.empty())
    return None;

  auto ParagraphText =
      dyn_cast<swift::markup::Text>(FirstParagraphChildren.front());
  if (!ParagraphText)
    return None;

  StringRef Name;
  StringRef Remainder;
  std::tie(Name, Remainder) = ParagraphText->getLiteralContent().split(':');
  Name = Name.rtrim();

  if (Name.empty())
    return None;

  ParagraphText->setLiteralContent(Remainder.ltrim());

  return swift::markup::ParamField::create(MC, Name, Children);
}

bool extractParameterOutline(
    swift::markup::MarkupContext &MC, swift::markup::List *L,
    SmallVectorImpl<swift::markup::ParamField *> &ParamFields) {
  SmallVector<swift::markup::MarkupASTNode *, 8> NormalItems;
  auto Children = L->getChildren();
  if (Children.empty())
    return false;

  for (auto Child : Children) {
    auto I = dyn_cast<swift::markup::Item>(Child);
    if (!I) {
      NormalItems.push_back(Child);
      continue;
    }

    auto ItemChildren = I->getChildren();
    if (ItemChildren.empty()) {
      NormalItems.push_back(Child);
      continue;
    }

    auto FirstChild = ItemChildren.front();
    auto FirstParagraph = dyn_cast<swift::markup::Paragraph>(FirstChild);
    if (!FirstParagraph) {
      NormalItems.push_back(Child);
      continue;
    }

    auto FirstParagraphChildren = FirstParagraph->getChildren();
    if (FirstParagraphChildren.empty()) {
      NormalItems.push_back(Child);
      continue;
    }

    auto HeadingText
        = dyn_cast<swift::markup::Text>(FirstParagraphChildren.front());
    if (!HeadingText) {
      NormalItems.push_back(Child);
      continue;
    }

    auto HeadingContent = HeadingText->getLiteralContent();
    if (!HeadingContent.rtrim().equals_insensitive("parameters:")) {
      NormalItems.push_back(Child);
      continue;
    }

    auto Rest = ArrayRef<swift::markup::MarkupASTNode *>(
        ItemChildren.begin() + 1, ItemChildren.end());
    if (Rest.empty()) {
      NormalItems.push_back(Child);
      continue;
    }

    for (auto Child : Rest) {
      auto SubList = dyn_cast<swift::markup::List>(Child);
      if (!SubList)
        continue;

      for (auto SubListChild : SubList->getChildren()) {
        auto Param = extractParamOutlineItem(MC, SubListChild);
        if (Param.has_value()) {
          ParamFields.push_back(Param.value());
        }
      }
    }
  }

  if (NormalItems.size() != Children.size()) {
    L->setChildren(NormalItems);
  }

  return NormalItems.empty();
}

bool extractSeparatedParams(
    swift::markup::MarkupContext &MC, swift::markup::List *L,
    SmallVectorImpl<swift::markup::ParamField *> &ParamFields) {
  SmallVector<swift::markup::MarkupASTNode *, 8> NormalItems;
  auto Children = L->getChildren();

  for (auto Child : Children) {
    auto I = dyn_cast<swift::markup::Item>(Child);
    if (!I) {
      NormalItems.push_back(Child);
      continue;
    }

    auto ItemChildren = I->getChildren();
    if (ItemChildren.empty()) {
      NormalItems.push_back(Child);
      continue;
    }

    auto FirstChild = ItemChildren.front();
    auto FirstParagraph = dyn_cast<swift::markup::Paragraph>(FirstChild);
    if (!FirstParagraph) {
      NormalItems.push_back(Child);
      continue;
    }

    auto FirstParagraphChildren = FirstParagraph->getChildren();
    if (FirstParagraphChildren.empty()) {
      NormalItems.push_back(Child);
      continue;
    }

    auto ParagraphText
        = dyn_cast<swift::markup::Text>(FirstParagraphChildren.front());
    if (!ParagraphText) {
      NormalItems.push_back(Child);
      continue;
    }

    StringRef ParameterPrefix("parameter ");
    auto ParagraphContent = ParagraphText->getLiteralContent();
    auto PotentialMatch = ParagraphContent.substr(0, ParameterPrefix.size());

    if (!PotentialMatch.startswith_insensitive(ParameterPrefix)) {
      NormalItems.push_back(Child);
      continue;
    }

    ParagraphContent = ParagraphContent.substr(ParameterPrefix.size());
    ParagraphText->setLiteralContent(ParagraphContent.ltrim());

    auto ParamField = extractParamOutlineItem(MC, I);
    if (ParamField.has_value())
      ParamFields.push_back(ParamField.value());
    else
      NormalItems.push_back(Child);
  }

  if (NormalItems.size() != Children.size())
    L->setChildren(NormalItems);

  return NormalItems.empty();
}

bool extractSimpleField(
    swift::markup::MarkupContext &MC, swift::markup::List *L,
    swift::markup::CommentParts &Parts,
    SmallVectorImpl<const swift::markup::MarkupASTNode *> &BodyNodes) {
  auto Children = L->getChildren();
  SmallVector<swift::markup::MarkupASTNode *, 8> NormalItems;
  for (auto Child : Children) {
    auto I = dyn_cast<swift::markup::Item>(Child);
    if (!I) {
      NormalItems.push_back(Child);
      continue;
    }

    auto ItemChildren = I->getChildren();
    if (ItemChildren.empty()) {
      NormalItems.push_back(Child);
      continue;
    }

    auto FirstParagraph
        = dyn_cast<swift::markup::Paragraph>(ItemChildren.front());
    if (!FirstParagraph) {
      NormalItems.push_back(Child);
      continue;
    }

    auto ParagraphChildren = FirstParagraph->getChildren();
    if (ParagraphChildren.empty()) {
      NormalItems.push_back(Child);
      continue;
    }

    auto ParagraphText
        = dyn_cast<swift::markup::Text>(ParagraphChildren.front());
    if (!ParagraphText) {
      NormalItems.push_back(Child);
      continue;
    }

    StringRef Tag;
    StringRef Remainder;
    std::tie(Tag, Remainder) = ParagraphText->getLiteralContent().split(':');
    Tag = Tag.ltrim().rtrim();
    Remainder = Remainder.ltrim();

    if (!swift::markup::isAFieldTag(Tag)) {
      NormalItems.push_back(Child);
      continue;
    }

    ParagraphText->setLiteralContent(Remainder);
    auto Field = swift::markup::createSimpleField(MC, Tag, ItemChildren);

    if (auto RF = dyn_cast<swift::markup::ReturnsField>(Field)) {
      Parts.ReturnsField = RF;
    } else if (auto TF = dyn_cast<swift::markup::ThrowsField>(Field)) {
      Parts.ThrowsField = TF;
    } else if (auto TF = dyn_cast<swift::markup::TagField>(Field)) {
      llvm::SmallString<64> Scratch;
      llvm::raw_svector_ostream OS(Scratch);
      printInlinesUnder(TF, OS);
      Parts.Tags.insert(MC.allocateCopy(OS.str()));
    } else if (auto LKF = dyn_cast<markup::LocalizationKeyField>(Field)) {
      Parts.LocalizationKeyField = LKF;
    } else {
      BodyNodes.push_back(Field);
    }
  }

  if (NormalItems.size() != Children.size())
    L->setChildren(NormalItems);

  return NormalItems.empty();
}
} // namespace

void swift::printBriefComment(RawComment RC, llvm::raw_ostream &OS) {
  markup::MarkupContext MC;
  markup::LineList LL = MC.getLineList(RC);
  auto *markupDoc = markup::parseDocument(MC, LL);

  auto children = markupDoc->getChildren();
  if (children.empty())
    return;
  auto FirstParagraph = dyn_cast<swift::markup::Paragraph>(children.front());
  if (!FirstParagraph)
    return;
  swift::markup::printInlinesUnder(FirstParagraph, OS);
}

swift::markup::CommentParts
swift::extractCommentParts(swift::markup::MarkupContext &MC,
                    swift::markup::MarkupASTNode *Node) {

  swift::markup::CommentParts Parts;
  auto Children = Node->getChildren();
  if (Children.empty())
    return Parts;

  auto FirstParagraph
      = dyn_cast<swift::markup::Paragraph>(Node->getChildren().front());
  if (FirstParagraph)
    Parts.Brief = FirstParagraph;

  SmallVector<const swift::markup::MarkupASTNode *, 4> BodyNodes;
  SmallVector<swift::markup::ParamField *, 8> ParamFields;

  // Look for special top-level lists
  size_t StartOffset = FirstParagraph == nullptr ? 0 : 1;
  for (auto C = Children.begin() + StartOffset; C != Children.end(); ++C) {
    if (auto L = dyn_cast<swift::markup::List>(*C)) {
      // Could be one of the following:
      // 1. A parameter outline:
      //    - Parameters:
      //      - x: ...
      //      - y: ...
      // 2. An exploded parameter list:
      //    - parameter x: ...
      //    - parameter y: ...
      // 3. Some other simple field, including "returns" (see SimpleFields.def)
      auto ListNowEmpty = extractParameterOutline(MC, L, ParamFields);
      ListNowEmpty |= extractSeparatedParams(MC, L, ParamFields);
      ListNowEmpty |= extractSimpleField(MC, L, Parts, BodyNodes);
      if (ListNowEmpty)
        continue; // This drops the empty list from the doc comment body.
    }
    BodyNodes.push_back(*C);
  }

  // Copy BodyNodes and ParamFields into the MarkupContext.
  Parts.BodyNodes = MC.allocateCopy(llvm::makeArrayRef(BodyNodes));
  Parts.ParamFields = MC.allocateCopy(llvm::makeArrayRef(ParamFields));

  for (auto Param : Parts.ParamFields) {
    auto ParamParts = extractCommentParts(MC, Param);
    Param->setParts(ParamParts);
  }

  return Parts;
}

DocComment *DocComment::create(const Decl *D, markup::MarkupContext &MC,
                               RawComment RC) {
  assert(!RC.isEmpty());
  swift::markup::LineList LL = MC.getLineList(RC);
  auto *Doc = swift::markup::parseDocument(MC, LL);
  auto Parts = extractCommentParts(MC, Doc);
  return new (MC) DocComment(D, Doc, Parts);
}

void DocComment::addInheritanceNote(swift::markup::MarkupContext &MC,
                                    TypeDecl *base) {
  auto text = MC.allocateCopy("This documentation comment was inherited from ");
  auto name = MC.allocateCopy(base->getNameStr());
  auto period = MC.allocateCopy(".");
  auto paragraph = markup::Paragraph::create(MC, {
    markup::Text::create(MC, text),
    markup::Code::create(MC, name),
    markup::Text::create(MC, period)});

  auto note = markup::NoteField::create(MC, {paragraph});

  SmallVector<const markup::MarkupASTNode *, 8> BodyNodes{
    Parts.BodyNodes.begin(), Parts.BodyNodes.end()};
  BodyNodes.push_back(note);
  Parts.BodyNodes = MC.allocateCopy(llvm::makeArrayRef(BodyNodes));
}

DocComment *swift::getSingleDocComment(swift::markup::MarkupContext &MC,
                                       const Decl *D) {
  PrettyStackTraceDecl StackTrace("parsing comment for", D);

  auto RC = D->getRawComment();
  if (RC.isEmpty())
    return nullptr;
  return DocComment::create(D, MC, RC);
}

namespace {
/// Helper class for finding the comment providing decl for either a brief or
/// raw comment.
template <typename Result>
class CommentProviderFinder final {
  using ResultWithDecl = std::pair<Result, const Decl *>;
  using VisitFnTy = Optional<Result>(*)(const Decl *);

  VisitFnTy VisitFn;

public:
  CommentProviderFinder(VisitFnTy visitFn) : VisitFn(visitFn) {}

private:
  Optional<ResultWithDecl> visit(const Decl *D) {
    // Adapt the provided visitor function to also return the decl.
    if (auto result = VisitFn(D))
      return {{*result, D}};
    return None;
  }

  Optional<ResultWithDecl> findOverriddenDecl(const ValueDecl *VD) {
    // Only applies to class member.
    if (!VD->getDeclContext()->getSelfClassDecl())
      return None;

    while (auto *baseDecl = VD->getOverriddenDecl()) {
      if (auto result = visit(baseDecl))
        return result;

      VD = baseDecl;
    }
    return None;
  }

  Optional<ResultWithDecl> findDefaultProvidedDecl(const ValueDecl *VD) {
    // Only applies to protocol extension member.
    auto *protocol = VD->getDeclContext()->getExtendedProtocolDecl();
    if (!protocol)
      return None;

    SmallVector<ValueDecl *, 2> members;
    protocol->lookupQualified(const_cast<ProtocolDecl *>(protocol),
                              DeclNameRef(VD->getName()),
                              NLOptions::NL_ProtocolMembers, members);

    Optional<ResultWithDecl> result;
    for (auto *member : members) {
      if (!isa<ProtocolDecl>(member->getDeclContext()) ||
          !member->isProtocolRequirement())
        continue;

      auto newResult = visit(member);
      if (!newResult)
        continue;

      if (result) {
        // Found two or more decls with doc-comment.
        return None;
      }
      result = newResult;
    }
    return result;
  }

  Optional<ResultWithDecl> findRequirementDecl(const ValueDecl *VD) {
    std::queue<const ValueDecl *> requirements;
    while (true) {
      for (auto *req : VD->getSatisfiedProtocolRequirements()) {
        if (auto result = visit(req))
          return result;

        requirements.push(req);
      }
      if (requirements.empty())
        return None;

      VD = requirements.front();
      requirements.pop();
    }
  }

public:
  Optional<ResultWithDecl> findCommentProvider(const Decl *D) {
    if (auto result = visit(D))
      return result;

    auto *VD = dyn_cast<ValueDecl>(D);
    if (!VD)
      return None;

    if (auto result = findOverriddenDecl(VD))
      return result;

    if (auto result = findDefaultProvidedDecl(VD))
      return result;

    if (auto result = findRequirementDecl(VD))
      return result;

    return None;
  }
};
} // end anonymous namespace

const Decl *swift::getDocCommentProvidingDecl(const Decl *D) {
  // Search for the first decl we see with a non-empty raw comment.
  auto finder = CommentProviderFinder<RawComment>(
      [](const Decl *D) -> Optional<RawComment> {
        auto comment = D->getRawComment();
        if (comment.isEmpty())
          return None;
        return comment;
      });

  auto result = finder.findCommentProvider(D);
  return result ? result->second : nullptr;
}

DocComment *swift::getCascadingDocComment(swift::markup::MarkupContext &MC,
                                          const Decl *D) {
  auto *docD = getDocCommentProvidingDecl(D);
  if (!docD)
    return nullptr;

  auto *doc = getSingleDocComment(MC, docD);
  assert(doc && "getDocCommentProvidingDecl() returned decl with no comment");

  // If the doc-comment is inherited from other decl, add a note about it.
  if (docD != D) {
    doc->setDecl(D);
    if (auto baseD = docD->getDeclContext()->getSelfNominalTypeDecl()) {
      doc->addInheritanceNote(MC, baseD);

      // If the doc is inherited from protocol requirement, associate the
      // requirement with the doc-comment.
      // FIXME: This is to keep the old behavior.
      if (isa<ProtocolDecl>(baseD))
        doc->setDecl(docD);
    }
  }

  return doc;
}

/// Retrieve the brief comment for a given decl \p D, without attempting to
/// walk any requirements or overrides.
static Optional<StringRef> getDirectBriefComment(const Decl *D) {
  if (!D->canHaveComment())
    return None;

  auto *ModuleDC = D->getDeclContext()->getModuleScopeContext();
  auto &Ctx = ModuleDC->getASTContext();

  // If we expect the comment to be in the swiftdoc, check for it if we loaded a
  // swiftdoc. If missing from the swiftdoc, we know it will not be in the
  // swiftsourceinfo either, so we can bail early.
  if (auto *Unit = dyn_cast<FileUnit>(ModuleDC)) {
    if (Unit->hasLoadedSwiftDoc()) {
      auto target = getDocCommentSerializationTargetFor(D);
      if (target == DocCommentSerializationTarget::SwiftDocAndSourceInfo) {
        auto C = Unit->getCommentForDecl(D);
        if (!C)
          return None;

        return C->Brief;
      }
    }
  }

  // Otherwise, parse the brief from the raw comment itself. This will look into
  // the swiftsourceinfo if needed.
  auto RC = D->getRawComment();
  if (RC.isEmpty())
    return None;

  SmallString<256> BriefStr;
  llvm::raw_svector_ostream OS(BriefStr);
  printBriefComment(RC, OS);
  return Ctx.AllocateCopy(BriefStr.str());
}

StringRef SemanticBriefCommentRequest::evaluate(Evaluator &evaluator,
                                                const Decl *D) const {
  // Perform a walk over the potential providers of the brief comment,
  // retrieving the first one we come across.
  CommentProviderFinder<StringRef> finder(getDirectBriefComment);
  auto result = finder.findCommentProvider(D);
  return result ? result->first : StringRef();
}

StringRef Decl::getSemanticBriefComment() const {
  if (!this->canHaveComment())
    return StringRef();

  auto &eval = getASTContext().evaluator;
  return evaluateOrDefault(eval, SemanticBriefCommentRequest{this},
                           StringRef());
}
