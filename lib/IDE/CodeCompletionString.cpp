//===--- CodeCompletionString.cpp -----------------------------------------===//
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

#include "swift/IDE/CodeCompletionString.h"
#include "swift/Basic/Range.h"

using namespace swift;
using namespace swift::ide;

CodeCompletionString::CodeCompletionString(ArrayRef<Chunk> Chunks) {
  std::uninitialized_copy(Chunks.begin(), Chunks.end(),
                          getTrailingObjects<Chunk>());
  NumChunks = Chunks.size();
}

CodeCompletionString *
CodeCompletionString::create(llvm::BumpPtrAllocator &Allocator,
                             ArrayRef<Chunk> Chunks) {
  void *CCSMem = Allocator.Allocate(totalSizeToAlloc<Chunk>(Chunks.size()),
                                    alignof(CodeCompletionString));
  return new (CCSMem) CodeCompletionString(Chunks);
}

llvm::Optional<unsigned> CodeCompletionString::getFirstTextChunkIndex(
    bool includeLeadingPunctuation) const {
  for (auto i : indices(getChunks())) {
    const Chunk &C = getChunks()[i];
    switch (C.getKind()) {
    using ChunkKind = Chunk::ChunkKind;
    case ChunkKind::Text:
      // Skip white-space only chunks.
      if (C.getText().find_first_not_of(" \r\n") == StringRef::npos)
        continue;
      return i;
    case ChunkKind::CallArgumentName:
    case ChunkKind::CallArgumentInternalName:
    case ChunkKind::ParameterDeclExternalName:
    case ChunkKind::ParameterDeclLocalName:
    case ChunkKind::ParameterDeclColon:
    case ChunkKind::GenericParameterClauseBegin:
    case ChunkKind::GenericRequirementClauseBegin:
    case ChunkKind::GenericParameterName:
    case ChunkKind::LeftParen:
    case ChunkKind::LeftBracket:
    case ChunkKind::Equal:
    case ChunkKind::DeclAttrParamKeyword:
    case ChunkKind::DeclAttrKeyword:
    case ChunkKind::Keyword:
    case ChunkKind::Attribute:
    case ChunkKind::BaseName:
    case ChunkKind::TypeIdSystem:
    case ChunkKind::TypeIdUser:
    case ChunkKind::CallArgumentBegin:
    case ChunkKind::DefaultArgumentClauseBegin:
    case ChunkKind::ParameterDeclBegin:
    case ChunkKind::EffectsSpecifierClauseBegin:
    case ChunkKind::DeclResultTypeClauseBegin:
    case ChunkKind::AttributeAndModifierListBegin:
      return i;
    case ChunkKind::Dot:
    case ChunkKind::ExclamationMark:
    case ChunkKind::QuestionMark:
      if (includeLeadingPunctuation)
        return i;
      continue;
    case ChunkKind::RightParen:
    case ChunkKind::RightBracket:
    case ChunkKind::LeftAngle:
    case ChunkKind::RightAngle:
    case ChunkKind::Ellipsis:
    case ChunkKind::Comma:
    case ChunkKind::Ampersand:
    case ChunkKind::Whitespace:
    case ChunkKind::AccessControlKeyword:
    case ChunkKind::OverrideKeyword:
    case ChunkKind::EffectsSpecifierKeyword:
    case ChunkKind::DeclIntroducer:
    case ChunkKind::CallArgumentColon:
    case ChunkKind::CallArgumentTypeBegin:
    case ChunkKind::CallArgumentType:
    case ChunkKind::CallArgumentDefaultBegin:
    case ChunkKind::CallArgumentClosureType:
    case ChunkKind::CallArgumentClosureExpr:
    case ChunkKind::ParameterDeclTypeBegin:
    case ChunkKind::DeclAttrParamColon:
    case ChunkKind::OptionalBegin:
    case ChunkKind::GenericParameterBegin:
    case ChunkKind::DynamicLookupMethodCallTail:
    case ChunkKind::OptionalMethodCallTail:
    case ChunkKind::TypeAnnotation:
    case ChunkKind::TypeAnnotationBegin:
      continue;

    case ChunkKind::BraceStmtWithCursor:
      llvm_unreachable("should have already extracted the text");
    }
  }
  return llvm::None;
}

StringRef
CodeCompletionString::getFirstTextChunk(bool includeLeadingPunctuation) const {
  llvm::Optional<unsigned> Idx =
      getFirstTextChunkIndex(includeLeadingPunctuation);
  if (Idx.has_value())
    return getChunks()[*Idx].getText();
  return StringRef();
}

void CodeCompletionString::print(raw_ostream &OS) const {

  unsigned PrevNestingLevel = 0;
  SmallVector<StringRef, 3> closeTags;

  auto chunks = getChunks();
  for (auto I = chunks.begin(), E = chunks.end(); I != E; ++I) {
    while (I->endsPreviousNestedGroup(PrevNestingLevel)) {
      OS << closeTags.pop_back_val();
      --PrevNestingLevel;
    }
    using ChunkKind = Chunk::ChunkKind;
    switch (I->getKind()) {
    case ChunkKind::AccessControlKeyword:
    case ChunkKind::DeclAttrKeyword:
    case ChunkKind::DeclAttrParamKeyword:
    case ChunkKind::OverrideKeyword:
    case ChunkKind::EffectsSpecifierKeyword:
    case ChunkKind::DeclIntroducer:
    case ChunkKind::Text:
    case ChunkKind::LeftParen:
    case ChunkKind::RightParen:
    case ChunkKind::LeftBracket:
    case ChunkKind::RightBracket:
    case ChunkKind::LeftAngle:
    case ChunkKind::RightAngle:
    case ChunkKind::Dot:
    case ChunkKind::Ellipsis:
    case ChunkKind::Comma:
    case ChunkKind::ExclamationMark:
    case ChunkKind::QuestionMark:
    case ChunkKind::Ampersand:
    case ChunkKind::Equal:
    case ChunkKind::Whitespace:
    case ChunkKind::Keyword:
    case ChunkKind::Attribute:
    case ChunkKind::BaseName:
    case ChunkKind::TypeIdSystem:
    case ChunkKind::TypeIdUser:
    case ChunkKind::CallArgumentName:
    case ChunkKind::CallArgumentColon:
    case ChunkKind::CallArgumentType:
    case ChunkKind::ParameterDeclExternalName:
    case ChunkKind::ParameterDeclLocalName:
    case ChunkKind::ParameterDeclColon:
    case ChunkKind::DeclAttrParamColon:
    case ChunkKind::GenericParameterName:
      if (I->isAnnotation())
        OS << "['" << I->getText() << "']";
      else
        OS << I->getText();
      break;
    case ChunkKind::CallArgumentInternalName:
      OS << "(" << I->getText() << ")";
      break;
    case ChunkKind::CallArgumentClosureType:
      OS << "##" << I->getText();
      break;
    case ChunkKind::OptionalBegin:
    case ChunkKind::CallArgumentBegin:
    case ChunkKind::GenericParameterBegin:
      OS << "{#";
      closeTags.emplace_back("#}");
      break;
    case ChunkKind::DynamicLookupMethodCallTail:
    case ChunkKind::OptionalMethodCallTail:
      OS << I->getText();
      break;
    case ChunkKind::GenericParameterClauseBegin:
    case ChunkKind::GenericRequirementClauseBegin:
    case ChunkKind::CallArgumentTypeBegin:
    case ChunkKind::DefaultArgumentClauseBegin:
    case ChunkKind::ParameterDeclBegin:
    case ChunkKind::EffectsSpecifierClauseBegin:
    case ChunkKind::DeclResultTypeClauseBegin:
    case ChunkKind::ParameterDeclTypeBegin:
    case ChunkKind::AttributeAndModifierListBegin:
    case ChunkKind::CallArgumentDefaultBegin:
      assert(I->getNestingLevel() == PrevNestingLevel + 1);
      closeTags.emplace_back("");
      break;
    case ChunkKind::TypeAnnotationBegin:
      OS << "[#";
      closeTags.emplace_back("#]");
      break;
    case ChunkKind::TypeAnnotation:
      OS << "[#" << I->getText() << "#]";
      break;
    case ChunkKind::CallArgumentClosureExpr:
      OS << " {" << I->getText() << "|}";
      break;
    case ChunkKind::BraceStmtWithCursor:
      OS << " {|}";
      break;
    }
    PrevNestingLevel = I->getNestingLevel();
  }
  while (PrevNestingLevel > 0) {
    OS << closeTags.pop_back_val();
    --PrevNestingLevel;
  }

  assert(closeTags.empty());
}

void CodeCompletionString::dump() const {
  llvm::raw_ostream &OS = llvm::errs();

  OS << "Chunks: \n";
  for (auto &chunk : getChunks()) {
    OS << "- ";
    for (unsigned i = 0, e = chunk.getNestingLevel(); i != e; ++i)
      OS << "| ";
    OS << "(";

    switch (chunk.getKind()) {
#define CASE(K)                                                                \
  case Chunk::ChunkKind::K:                                                    \
    OS << #K;                                                                  \
    break;
      CASE(AccessControlKeyword)
      CASE(DeclAttrKeyword)
      CASE(DeclAttrParamKeyword)
      CASE(OverrideKeyword)
      CASE(EffectsSpecifierKeyword)
      CASE(DeclIntroducer)
      CASE(Keyword)
      CASE(Attribute)
      CASE(Text)
      CASE(BaseName)
      CASE(OptionalBegin)
      CASE(LeftParen)
      CASE(RightParen)
      CASE(LeftBracket)
      CASE(RightBracket)
      CASE(LeftAngle)
      CASE(RightAngle)
      CASE(Dot)
      CASE(Ellipsis)
      CASE(Comma)
      CASE(ExclamationMark)
      CASE(QuestionMark)
      CASE(Ampersand)
      CASE(Equal)
      CASE(Whitespace)
      CASE(GenericParameterClauseBegin)
      CASE(GenericRequirementClauseBegin)
      CASE(GenericParameterBegin)
      CASE(GenericParameterName)
      CASE(CallArgumentBegin)
      CASE(CallArgumentName)
      CASE(CallArgumentInternalName)
      CASE(CallArgumentColon)
      CASE(DeclAttrParamColon)
      CASE(CallArgumentType)
      CASE(CallArgumentTypeBegin)
      CASE(CallArgumentDefaultBegin)
      CASE(TypeIdSystem)
      CASE(TypeIdUser)
      CASE(CallArgumentClosureType)
      CASE(CallArgumentClosureExpr)
      CASE(DynamicLookupMethodCallTail)
      CASE(OptionalMethodCallTail)
      CASE(ParameterDeclBegin)
      CASE(ParameterDeclExternalName)
      CASE(ParameterDeclLocalName)
      CASE(ParameterDeclColon)
      CASE(ParameterDeclTypeBegin)
      CASE(DefaultArgumentClauseBegin)
      CASE(EffectsSpecifierClauseBegin)
      CASE(DeclResultTypeClauseBegin)
      CASE(AttributeAndModifierListBegin)
      CASE(TypeAnnotation)
      CASE(TypeAnnotationBegin)
      CASE(BraceStmtWithCursor)
    }
    if (chunk.isAnnotation())
      OS << " [annotation]";
    if (chunk.hasText()) {
      OS << " \"";
      OS.write_escaped(chunk.getText());
      OS << "\"";
    }
    OS << ")\n";
  }
}
