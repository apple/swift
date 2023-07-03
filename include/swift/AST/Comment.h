//===--- Comment.h - Swift-specific comment parsing -------------*- C++ -*-===//
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

#ifndef SWIFT_AST_COMMENT_H
#define SWIFT_AST_COMMENT_H

#include "swift/Markup/Markup.h"
#include "llvm/ADT/Optional.h"

namespace swift {
class Decl;
class TypeDecl;
struct RawComment;

class DocComment {
  const Decl *D;
  swift::markup::Document *Doc = nullptr;
  swift::markup::CommentParts Parts;

  DocComment(const Decl *D, swift::markup::Document *Doc,
             swift::markup::CommentParts Parts)
      : D(D), Doc(Doc), Parts(Parts) {}

public:
  static DocComment *create(const Decl *D, swift::markup::MarkupContext &MC,
                            RawComment RC);

  void addInheritanceNote(swift::markup::MarkupContext &MC, TypeDecl *base);

  const Decl *getDecl() const { return D; }
  void setDecl(const Decl *D) { this->D = D; }

  const swift::markup::Document *getDocument() const { return Doc; }

  swift::markup::CommentParts getParts() const {
    return Parts;
  }

  ArrayRef<StringRef> getTags() const {
    return llvm::makeArrayRef(Parts.Tags.begin(), Parts.Tags.end());
  }

  llvm::Optional<const swift::markup::Paragraph *> getBrief() const {
    return Parts.Brief;
  }

  llvm::Optional<const swift::markup::ReturnsField *> getReturnsField() const {
    return Parts.ReturnsField;
  }

  llvm::Optional<const swift::markup::ThrowsField *> getThrowsField() const {
    return Parts.ThrowsField;
  }

  ArrayRef<const swift::markup::ParamField *> getParamFields() const {
    return Parts.ParamFields;
  }

  ArrayRef<const swift::markup::MarkupASTNode *> getBodyNodes() const {
    return Parts.BodyNodes;
  }

  llvm::Optional<const markup::LocalizationKeyField *>
  getLocalizationKeyField() const {
    return Parts.LocalizationKeyField;
  }

  bool isEmpty() const {
    return Parts.isEmpty();
  }

  // Only allow allocation using the allocator in MarkupContext or by
  // placement new.
  void *operator new(size_t Bytes, swift::markup::MarkupContext &MC,
                     unsigned Alignment = alignof(DocComment));
  void *operator new(size_t Bytes, void *Mem) {
    assert(Mem);
    return Mem;
  }

  // Make vanilla new/delete illegal.
  void *operator new(size_t Bytes) = delete;
  void operator delete(void *Data) = delete;
};

/// Get a parsed documentation comment for the declaration, if there is one.
DocComment *getSingleDocComment(swift::markup::MarkupContext &Context,
                                const Decl *D);

/// Get the declaration that actually provides a doc comment for another.
const Decl *getDocCommentProvidingDecl(const Decl *D);

/// Attempt to get a doc comment from the declaration, or other inherited
/// sources, like from base classes or protocols.
DocComment *getCascadingDocComment(swift::markup::MarkupContext &MC,
                                   const Decl *D);

/// Extract comments parts from the given Markup node.
swift::markup::CommentParts
extractCommentParts(swift::markup::MarkupContext &MC,
                    swift::markup::MarkupASTNode *Node);

/// Extract brief comment from \p RC, and print it to \p OS .
void printBriefComment(RawComment RC, llvm::raw_ostream &OS);

/// Describes the intended serialization target for a doc comment.
enum class DocCommentSerializationTarget : uint8_t {
  /// The doc comment should not be serialized.
  None = 0,

  /// The doc comment should only be serialized in the 'swiftsourceinfo'.
  SourceInfoOnly,

  /// The doc comment should be serialized in both the 'swiftdoc' and
  /// 'swiftsourceinfo'.
  SwiftDocAndSourceInfo,
};

/// Retrieve the expected serialization target for a documentation comment
/// attached to the given decl.
DocCommentSerializationTarget
getDocCommentSerializationTargetFor(const Decl *D);

} // namespace swift

#endif // LLVM_SWIFT_AST_COMMENT_H
