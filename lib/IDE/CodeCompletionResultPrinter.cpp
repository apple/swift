//===--- CodeCompletionResultPrinter.cpp ----------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/IDE/CodeCompletionResultPrinter.h"
#include "swift/AST/ASTPrinter.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/StringExtras.h"
#include "swift/IDE/CodeCompletion.h"
#include "swift/Markup/XMLUtils.h"
#include "llvm/Support/raw_ostream.h"

using namespace swift;
using namespace swift::ide;

using ChunkKind = CodeCompletionString::Chunk::ChunkKind;
using ChunkIter = ArrayRef<CodeCompletionString::Chunk>::iterator;

/// Advance \p i to just the past the group \i currently points.
void skipToEndOfCurrentNestedGroup(ChunkIter &i, const ChunkIter e) {
  auto level = i->getNestingLevel();
  assert(i != e);
  do { ++i; } while (i != e && !i->endsPreviousNestedGroup(level));
}

void swift::ide::printCodeCompletionResultDescription(
    const CodeCompletionResult &result,

    raw_ostream &OS, bool leadingPunctuation) {
  auto str = result.getCompletionString();
  bool isOperator = result.isOperator();

  auto FirstTextChunk = str->getFirstTextChunkIndex(leadingPunctuation);
  int TextSize = 0;
  if (FirstTextChunk.has_value()) {
    auto Chunks = str->getChunks().slice(*FirstTextChunk);
    auto I = Chunks.begin(), E = Chunks.end();
    while (I != E) {
      const auto &C = *I;

      using ChunkKind = CodeCompletionString::Chunk::ChunkKind;

      if (C.is(ChunkKind::TypeAnnotation) ||
          C.is(ChunkKind::CallArgumentClosureType) ||
          C.is(ChunkKind::CallArgumentClosureExpr) ||
          C.is(ChunkKind::DeclIntroducer) ||
          C.is(ChunkKind::Whitespace)) {
        ++I;
        continue;
      }

      // Skip the attribute/modifier list.
      if (I->is(ChunkKind::AttributeAndModifierListBegin)) {
        skipToEndOfCurrentNestedGroup(I, E);
        continue;
      }

      // Skip the declaration introducer and the following ' '.
      if (I->is(ChunkKind::DeclIntroducer)) {
        ++I;
        continue;
      }
      if (I->is(ChunkKind::Text) && I->getText() == " " &&
          I != Chunks.begin() && (I-1)->is(ChunkKind::DeclIntroducer)) {
        ++I;
        continue;
      }

      // Skip TypeAnnotation group.
      if (C.is(ChunkKind::TypeAnnotationBegin)) {
        skipToEndOfCurrentNestedGroup(I, E);
        continue;
      }

      if (isOperator && C.is(ChunkKind::CallArgumentType)) {
        ++I;
        continue;
      }
      if (isOperator && C.is(ChunkKind::CallArgumentTypeBegin)) {
        skipToEndOfCurrentNestedGroup(I, E);
        continue;
      }
      
      if (C.hasText()) {
        TextSize += C.getText().size();
        OS << C.getText();
      }
      ++I;
    }
  }
  assert((TextSize > 0) &&
         "code completion result should have non-empty description!");
}

namespace {
class AnnotatingResultPrinter {
  raw_ostream &OS;

  /// Print \p content enclosing with \p tag.
  void printWithTag(StringRef tag, StringRef content) {
    // Trim whitepsaces around the non-whitespace characters.
    // (i.e. "  something   " -> "  <tag>something</tag>   ".
    auto ltrimIdx = content.find_first_not_of(' ');
    auto rtrimIdx = content.find_last_not_of(' ') + 1;
    assert(ltrimIdx != StringRef::npos && rtrimIdx != StringRef::npos &&
           "empty or whitespace only element");

    OS << content.substr(0, ltrimIdx);
    OS << "<" << tag << ">";
    swift::markup::appendWithXMLEscaping(
        OS, content.substr(ltrimIdx, rtrimIdx - ltrimIdx));
    OS << "</" << tag << ">";
    OS << content.substr(rtrimIdx);
  }

  void printTextChunk(CodeCompletionString::Chunk C) {
    if (!C.hasText())
      return;

    switch (C.getKind()) {
    case ChunkKind::Keyword:
    case ChunkKind::OverrideKeyword:
    case ChunkKind::AccessControlKeyword:
    case ChunkKind::EffectsSpecifierKeyword:
    case ChunkKind::DeclIntroducer:
      printWithTag("keyword", C.getText());
      break;
    case ChunkKind::DeclAttrKeyword:
    case ChunkKind::Attribute:
      printWithTag("attribute", C.getText());
      break;
    case ChunkKind::BaseName:
      printWithTag("name", C.getText());
      break;
    case ChunkKind::TypeIdSystem:
      printWithTag("typeid.sys", C.getText());
      break;
    case ChunkKind::TypeIdUser:
      printWithTag("typeid.user", C.getText());
      break;
    case ChunkKind::CallArgumentName:
      printWithTag("callarg.label", C.getText());
      break;
    case ChunkKind::CallArgumentInternalName:
      printWithTag("callarg.param", C.getText());
      break;
    case ChunkKind::ParameterDeclExternalName:
      printWithTag("param.label", C.getText());
      break;
    case ChunkKind::ParameterDeclLocalName:
      printWithTag("param.param", C.getText());
      break;
    case ChunkKind::TypeAnnotation:
    case ChunkKind::CallArgumentClosureType:
    case ChunkKind::CallArgumentClosureExpr:
    case ChunkKind::Whitespace:
      // ignore;
      break;
    default:
      swift::markup::appendWithXMLEscaping(OS, C.getText());
      break;
    }
  }

  /// Print the current chunk group \p i currently points. Advances \p i to
  /// just past the group.
  void printNestedGroup(StringRef tag, ChunkIter &i, const ChunkIter e) {
    assert(i != e && !i->hasText() &&
           CodeCompletionString::Chunk::chunkStartsNestedGroup(i->getKind()));
    auto nestingLevel = i->getNestingLevel();

    // Skip the "Begin" chunk.
    ++i;

    // Self-closing tag for an empty element.
    if (i == e || i->endsPreviousNestedGroup(nestingLevel)) {
      if (!tag.empty())
        OS << "<" << tag << "/>";
      return;
    }

    if (!tag.empty())
      OS << "<" << tag << ">";
    do {
      if (i->is(ChunkKind::CallArgumentTypeBegin)) {
        printNestedGroup("callarg.type", i, e);
        continue;
      }
      if (i->is(ChunkKind::CallArgumentDefaultBegin)) {
        printNestedGroup("callarg.default", i, e);
        continue;
      }
      if (i->is(ChunkKind::ParameterDeclTypeBegin)) {
        printNestedGroup("param.type", i, e);
        continue;
      }
      printTextChunk(*i);
      ++i;
    } while (i != e && !i->endsPreviousNestedGroup(nestingLevel));
    if (!tag.empty())
      OS << "</" << tag << ">";
  }

public:
  AnnotatingResultPrinter(raw_ostream &OS) : OS(OS) {}

  void printDescription(const CodeCompletionResult &result, bool leadingPunctuation) {
    auto str = result.getCompletionString();
    bool isOperator = result.isOperator();

    auto FirstTextChunk = str->getFirstTextChunkIndex(leadingPunctuation);
    if (FirstTextChunk.has_value()) {
      auto chunks = str->getChunks().slice(*FirstTextChunk);
      auto i = chunks.begin(), e = chunks.end();
      while (i != e) {

        // Skip the attribute/modifier list.
        if (i->is(ChunkKind::AttributeAndModifierListBegin)) {
          skipToEndOfCurrentNestedGroup(i, e);
          continue;
        }

        // Skip the declaration introducer and the following ' '.
        if (i->is(ChunkKind::DeclIntroducer)) {
          ++i;
          continue;
        }
        if (i->is(ChunkKind::Text) && i->getText() == " " &&
            i != chunks.begin() && (i-1)->is(ChunkKind::DeclIntroducer)) {
          ++i;
          continue;
        }

        // Skip the type annotation.
        if (i->is(ChunkKind::TypeAnnotationBegin)) {
          skipToEndOfCurrentNestedGroup(i, e);
          continue;
        }

        // Print call argument group.
        if (i->is(ChunkKind::CallArgumentBegin)) {
          if (isOperator) {
            // Don't print call argument clause for operators.
            skipToEndOfCurrentNestedGroup(i, e);
            continue;
          }
          printNestedGroup("callarg", i, e);
          continue;
        }
        // Print call argument group.
        if (i->is(ChunkKind::ParameterDeclBegin)) {
          printNestedGroup("param", i, e);
          continue;
        }

        if (isOperator && i->is(ChunkKind::CallArgumentType)) {
          ++i;
          continue;
        }

        printTextChunk(*i);
        ++i;
      }
    }
  }

  void printTypeName(const CodeCompletionResult &result) {
    auto Chunks = result.getCompletionString()->getChunks();

    auto i = Chunks.begin(), e = Chunks.end();
    while (i != e) {
      if (i->is(ChunkKind::TypeAnnotation)) {
        OS << i->getText();
        ++i;
        continue;
      }
      if (i->is(ChunkKind::TypeAnnotationBegin)) {
        printNestedGroup("", i, e);
        continue;
      }

      // Ignore others.
      ++i;
    }
  }
};

} // namespace

void swift::ide::printCodeCompletionResultDescriptionAnnotated(
    const CodeCompletionResult &Result, raw_ostream &OS,
    bool leadingPunctuation) {
  AnnotatingResultPrinter printer(OS);
  printer.printDescription(Result, leadingPunctuation);
}

void swift::ide::printCodeCompletionResultTypeName(const CodeCompletionResult &Result,
                                                   llvm::raw_ostream &OS) {
  auto Chunks = Result.getCompletionString()->getChunks();
  auto i = Chunks.begin(), e = Chunks.end();

  while (i != e) {
    if (i->is(CodeCompletionString::Chunk::ChunkKind::TypeAnnotation)) {
      OS << i->getText();
      ++i;
      continue;
    }

    if (i->is(CodeCompletionString::Chunk::ChunkKind::TypeAnnotationBegin)) {
      auto nestingLevel = i->getNestingLevel();
      ++i;
      while (i != e && !i->endsPreviousNestedGroup(nestingLevel)) {
        if (i->hasText())
          OS << i->getText();
        ++i;
      }
      continue;
    }

    // Ignore others.
    ++i;
  }
}

void swift::ide::printCodeCompletionResultTypeNameAnnotated(const CodeCompletionResult &Result, llvm::raw_ostream &OS) {
  AnnotatingResultPrinter printer(OS);
  printer.printTypeName(Result);
}

/// Provide the text for the call parameter, including constructing a typed
/// editor placeholder for it.
static void
constructTextForCallParam(ArrayRef<CodeCompletionString::Chunk> ParamGroup,
                          raw_ostream &OS) {
  assert(ParamGroup.front().is(ChunkKind::CallArgumentBegin));

  for (; !ParamGroup.empty(); ParamGroup = ParamGroup.slice(1)) {
    auto &C = ParamGroup.front();
    if (C.isAnnotation())
      continue;
    if (C.is(ChunkKind::CallArgumentInternalName) ||
        C.is(ChunkKind::CallArgumentType) ||
        C.is(ChunkKind::CallArgumentTypeBegin) ||
        C.is(ChunkKind::CallArgumentClosureExpr)) {
      break;
    }
    if (!C.hasText())
      continue;
    OS << C.getText();
  }

  SmallString<32> DisplayString;
  SmallString<32> TypeString;
  SmallString<32> ExpansionTypeString;

  for (auto i = ParamGroup.begin(), e = ParamGroup.end(); i != e; ++i) {
    auto &C = *i;
    if (C.is(ChunkKind::CallArgumentTypeBegin)) {
      assert(TypeString.empty());
      auto nestingLevel = C.getNestingLevel();
      ++i;
      for (; i != e; ++i) {
        if (i->endsPreviousNestedGroup(nestingLevel))
          break;
        if (!i->isAnnotation() && i->hasText()) {
          TypeString += i->getText();
          DisplayString += i->getText();
        }
      }
      --i;
      continue;
    }
    if (C.is(ChunkKind::CallArgumentClosureType)) {
      assert(ExpansionTypeString.empty());
      ExpansionTypeString = C.getText();
      continue;
    }
    if (C.is(ChunkKind::CallArgumentType)) {
      assert(TypeString.empty());
      TypeString = C.getText();
    }
    if (C.is(ChunkKind::CallArgumentClosureExpr)) {
      // We have a closure expression, so provide it directly instead of in
      // a placeholder.
      OS << "{";
      if (!C.getText().empty())
        OS << " " << C.getText();
      OS << "\n" << getCodePlaceholder() << "\n}";
      return;
    }
    if (C.isAnnotation() || !C.hasText())
      continue;
    DisplayString += C.getText();
  }

  StringRef Display = DisplayString.str();
  StringRef Type = TypeString.str();
  StringRef ExpansionType = ExpansionTypeString.str();
  if (ExpansionType.empty())
    ExpansionType = Type;

  OS << "<#T##" << Display;
  if (Display == Type && Display == ExpansionType) {
    // Short version, display and type are the same.
  } else if (!Type.empty()) {
    OS << "##" << Type;
    if (ExpansionType != Type)
      OS << "##" << ExpansionType;
  }
  OS << "#>";
}

void swift::ide::printCodeCompletionResultSourceText(
    const CodeCompletionResult &Result, llvm::raw_ostream &OS) {
  auto Chunks = Result.getCompletionString()->getChunks();
  for (size_t i = 0; i < Chunks.size(); ++i) {
    auto &C = Chunks[i];
    if (C.is(ChunkKind::BraceStmtWithCursor)) {
      OS << " {\n" << getCodePlaceholder() << "\n}";
      continue;
    }
    if (C.is(ChunkKind::CallArgumentBegin)) {
      size_t Start = i++;
      for (; i < Chunks.size(); ++i) {
        if (Chunks[i].endsPreviousNestedGroup(C.getNestingLevel()))
          break;
      }
      constructTextForCallParam(Chunks.slice(Start, i - Start), OS);
      --i;
      continue;
    }
    if (C.is(ChunkKind::TypeAnnotationBegin)) {
      // Skip type annotation structure.
      auto level = C.getNestingLevel();
      do {
        ++i;
      } while (i != Chunks.size() && !Chunks[i].endsPreviousNestedGroup(level));
      --i;
    }
    if (!C.isAnnotation() && C.hasText()) {
      OS << C.getText();
    }
  }
}

static void printCodeCompletionResultFilterName(
    const CodeCompletionString *str, llvm::raw_ostream &OS) {
  // FIXME: we need a more uniform way to handle operator completions.
  if (str->getChunks().size() == 1 && str->getChunks()[0].is(ChunkKind::Dot)) {
    OS << ".";
    return;
  } else if (str->getChunks().size() == 2 &&
             str->getChunks()[0].is(ChunkKind::QuestionMark) &&
             str->getChunks()[1].is(ChunkKind::Dot)) {
    OS << "?.";
    return;
  }

  auto FirstTextChunk = str->getFirstTextChunkIndex();
  if (FirstTextChunk.has_value()) {
    auto chunks = str->getChunks().slice(*FirstTextChunk);
    auto i = chunks.begin(), e = chunks.end();
    while (i != e) {
      auto &C = *i;

      if (C.is(ChunkKind::BraceStmtWithCursor))
        break; // Don't include brace-stmt in filter name.

      if (C.is(ChunkKind::Equal)) {
        OS << C.getText();
        break;
      }

      bool shouldPrint = !C.isAnnotation();
      switch (C.getKind()) {
      case ChunkKind::TypeAnnotation:
      case ChunkKind::CallArgumentInternalName:
      case ChunkKind::CallArgumentClosureType:
      case ChunkKind::CallArgumentClosureExpr:
      case ChunkKind::CallArgumentType:
      case ChunkKind::ParameterDeclLocalName:
      case ChunkKind::DeclAttrParamColon:
      case ChunkKind::Comma:
      case ChunkKind::Whitespace:
      case ChunkKind::Ellipsis:
      case ChunkKind::Ampersand:
      case ChunkKind::OptionalMethodCallTail:
      case ChunkKind::DeclIntroducer:
        ++i;
        continue;
      case ChunkKind::ParameterDeclExternalName:
        // Skip '_' parameter external name.
        shouldPrint = shouldPrint && C.hasText() && C.getText() != "_";
        break;
      case ChunkKind::CallArgumentTypeBegin:
      case ChunkKind::ParameterDeclTypeBegin:
      case ChunkKind::TypeAnnotationBegin:
      case ChunkKind::DefaultArgumentClauseBegin:
      case ChunkKind::GenericParameterClauseBegin:
      case ChunkKind::EffectsSpecifierClauseBegin:
      case ChunkKind::GenericRequirementClauseBegin:
      case ChunkKind::DeclResultTypeClauseBegin:
      case ChunkKind::AttributeAndModifierListBegin: {
        // Skip call parameter type or type annotation structure.
        skipToEndOfCurrentNestedGroup(i, e);
        continue;
      }
      case ChunkKind::CallArgumentColon:
      case ChunkKind::ParameterDeclColon:
        // Since we don't add the type, also don't add the space after ':'.
        OS << ":";
        ++i;
        continue;
      case ChunkKind::Text:
        // Ignore " " after call argument labels.
        if (i->getText() == " " && i != chunks.begin() &&
            ((i-1)->is(ChunkKind::CallArgumentName) ||
             (i-1)->is(ChunkKind::ParameterDeclExternalName) ||
             (i-1)->is(ChunkKind::DeclIntroducer))) {
          ++i;
          continue;
        }
        break;
      default:
        break;
      }

      if (C.hasText() && shouldPrint)
        OS << C.getText();
      ++i;
    }
  }
}

NullTerminatedStringRef swift::ide::getCodeCompletionResultFilterName(
    const CodeCompletionString *Str, llvm::BumpPtrAllocator &Allocator) {
  SmallString<32> buf;
  llvm::raw_svector_ostream OS(buf);
  printCodeCompletionResultFilterName(Str, OS);
  return NullTerminatedStringRef(buf, Allocator);
}
