//===--- CodeCompletionStringPrinter.cpp ----------------------------------===//
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

#include "swift/IDE/CodeCompletionStringPrinter.h"
#include "CodeCompletionResultBuilder.h"
#include "swift/AST/Module.h"

using namespace swift;
using namespace swift::ide;

using ChunkKind = CodeCompletionString::Chunk::ChunkKind;

llvm::Optional<ChunkKind>
CodeCompletionStringPrinter::getChunkKindForPrintNameContext(
    PrintNameContext context) const {
  switch (context) {
  case PrintNameContext::Keyword:
    if (isCurrentStructureKind(PrintStructureKind::EffectsSpecifiers)) {
      return ChunkKind::EffectsSpecifierKeyword;
    }
    return ChunkKind::Keyword;
  case PrintNameContext::IntroducerKeyword:
    return ChunkKind::DeclIntroducer;
  case PrintNameContext::Attribute:
    return ChunkKind::Attribute;
  case PrintNameContext::FunctionParameterExternal:
    if (isInType()) {
      return llvm::None;
    }
    return ChunkKind::ParameterDeclExternalName;
  case PrintNameContext::FunctionParameterLocal:
    if (isInType()) {
      return llvm::None;
    }
    return ChunkKind::ParameterDeclLocalName;
  default:
    return llvm::None;
  }
}

llvm::Optional<ChunkKind>
CodeCompletionStringPrinter::getChunkKindForStructureKind(
    PrintStructureKind Kind) const {
  switch (Kind) {
  case PrintStructureKind::FunctionParameter:
    if (isInType()) {
      return llvm::None;
    }
    return ChunkKind::ParameterDeclBegin;
  case PrintStructureKind::DefaultArgumentClause:
    return ChunkKind::DefaultArgumentClauseBegin;
  case PrintStructureKind::DeclGenericParameterClause:
    return ChunkKind::GenericParameterClauseBegin;
  case PrintStructureKind::DeclGenericRequirementClause:
    return ChunkKind::GenericRequirementClauseBegin;
  case PrintStructureKind::EffectsSpecifiers:
    return ChunkKind::EffectsSpecifierClauseBegin;
  case PrintStructureKind::DeclResultTypeClause:
    return ChunkKind::DeclResultTypeClauseBegin;
  case PrintStructureKind::FunctionParameterType:
    return ChunkKind::ParameterDeclTypeBegin;
  default:
    return llvm::None;
  }
}

void CodeCompletionStringPrinter::startNestedGroup(ChunkKind Kind) {
  flush();
  Builder.CurrentNestingLevel++;
  Builder.addSimpleChunk(Kind);
}

void CodeCompletionStringPrinter::endNestedGroup() {
  flush();
  Builder.CurrentNestingLevel--;
}

void CodeCompletionStringPrinter::flush() {
  if (Buffer.empty())
    return;
  Builder.addChunkWithText(CurrChunkKind, Buffer);
  Buffer.clear();
}

/// Start \c AttributeAndModifierListBegin group. This must be called before
/// any attributes/modifiers printed to the output when printing an override
/// compleion.
void CodeCompletionStringPrinter::startPreamble() {
  assert(!InPreamble);
  startNestedGroup(ChunkKind::AttributeAndModifierListBegin);
  InPreamble = true;
}

void CodeCompletionStringPrinter::endPremable() {
  if (!InPreamble)
    return;
  InPreamble = false;
  endNestedGroup();
}

void CodeCompletionStringPrinter::printText(StringRef Text) {
  // Detect ': ' and ', ' in parameter clauses.
  // FIXME: Is there a better way?
  if (isCurrentStructureKind(PrintStructureKind::FunctionParameter) &&
      Text == ": ") {
    setNextChunkKind(ChunkKind::ParameterDeclColon);
  } else if (isCurrentStructureKind(
                 PrintStructureKind::FunctionParameterList) &&
             Text == ", ") {
    setNextChunkKind(ChunkKind::Comma);
  }

  if (CurrChunkKind != NextChunkKind) {
    // If the next desired kind is different from the current buffer, flush
    // the current buffer.
    flush();
    CurrChunkKind = NextChunkKind;
  }
  Buffer.append(Text);
}

void CodeCompletionStringPrinter::printTypeRef(Type T, const TypeDecl *TD,
                                               Identifier Name,
                                               PrintNameContext NameContext) {

  NextChunkKind = TD->getModuleContext()->isNonUserModule()
                      ? ChunkKind::TypeIdSystem
                      : ChunkKind::TypeIdUser;

  ASTPrinter::printTypeRef(T, TD, Name, NameContext);
  NextChunkKind = ChunkKind::Text;
}

void CodeCompletionStringPrinter::printDeclLoc(const Decl *D) {
  endPremable();
  setNextChunkKind(ChunkKind::BaseName);
}

void CodeCompletionStringPrinter::printDeclNameEndLoc(const Decl *D) {
  setNextChunkKind(ChunkKind::Text);
}

void CodeCompletionStringPrinter::printNamePre(PrintNameContext context) {
  if (context == PrintNameContext::IntroducerKeyword)
    endPremable();
  if (auto Kind = getChunkKindForPrintNameContext(context))
    setNextChunkKind(*Kind);
}

void CodeCompletionStringPrinter::printNamePost(PrintNameContext context) {
  if (getChunkKindForPrintNameContext(context))
    setNextChunkKind(ChunkKind::Text);
}

void CodeCompletionStringPrinter::printTypePre(const TypeLoc &TL) {
  ++TypeDepth;
}

void CodeCompletionStringPrinter::printTypePost(const TypeLoc &TL) {
  assert(TypeDepth > 0);
  --TypeDepth;
}

void CodeCompletionStringPrinter::printStructurePre(PrintStructureKind Kind,
                                                    const Decl *D) {
  StructureStack.push_back(Kind);

  if (auto chunkKind = getChunkKindForStructureKind(Kind))
    startNestedGroup(*chunkKind);
}

void CodeCompletionStringPrinter::printStructurePost(PrintStructureKind Kind,
                                                     const Decl *D) {
  if (getChunkKindForStructureKind(Kind))
    endNestedGroup();

  assert(Kind == StructureStack.back());
  StructureStack.pop_back();
}
