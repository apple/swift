//===--- PrintingDiagnosticConsumer.cpp - Print Text Diagnostics ----------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file implements the PrintingDiagnosticConsumer class.
//
//===----------------------------------------------------------------------===//

#include "swift/Frontend/PrintingDiagnosticConsumer.h"
#include "swift/AST/ASTBridging.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/DiagnosticsCommon.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/ColorUtils.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Bridging/ASTGen.h"
#include "swift/Markup/Markup.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/FormatAdapters.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <cmath>

using namespace swift;
using namespace swift::markup;

namespace {
// MARK: Markdown Printing
    class TerminalMarkupPrinter : public MarkupASTVisitor<TerminalMarkupPrinter> {
      llvm::raw_ostream &OS;
      unsigned Indent;
      unsigned ShouldBold;

      void indent(unsigned Amount = 2) { Indent += Amount; }

      void dedent(unsigned Amount = 2) {
        assert(Indent >= Amount && "dedent without matching indent");
        Indent -= Amount;
      }

      void bold() {
        ++ShouldBold;
        updateFormatting();
      }

      void unbold() {
        assert(ShouldBold > 0 && "unbolded without matching bold");
        --ShouldBold;
        updateFormatting();
      }

      void updateFormatting() {
        OS.resetColor();
        if (ShouldBold > 0)
          OS.changeColor(raw_ostream::Colors::SAVEDCOLOR, true);
      }

      void print(StringRef Str) {
        for (auto c : Str) {
          OS << c;
          if (c == '\n')
            for (unsigned i = 0; i < Indent; ++i)
              OS << ' ';
        }
      }

    public:
      TerminalMarkupPrinter(llvm::raw_ostream &OS)
          : OS(OS), Indent(0), ShouldBold(0) {}

      void printNewline() { print("\n"); }

      void visitDocument(const Document *D) {
        for (const auto *Child : D->getChildren()) {
          if (Child->getKind() == markup::ASTNodeKind::Paragraph) {
            // Add a newline before top-level paragraphs
            printNewline();
          }
          visit(Child);
        }
      }

      void visitInlineAttributes(const InlineAttributes *A) {
        print("^[");
        for (const auto *Child : A->getChildren())
          visit(Child);
        print("](");
        print(A->getAttributes());
        print(")");
      }

      void visitBlockQuote(const BlockQuote *BQ) {
        indent();
        printNewline();
        for (const auto *Child : BQ->getChildren())
          visit(Child);
        dedent();
      }

      void visitList(const List *BL) {
        indent();
        printNewline();
        for (const auto *Child : BL->getChildren())
          visit(Child);
        dedent();
      }

      void visitItem(const Item *I) {
        print("- ");
        for (const auto *N : I->getChildren())
          visit(N);
      }

      void visitCodeBlock(const CodeBlock *CB) {
        indent();
        printNewline();
        print(CB->getLiteralContent());
        dedent();
      }

      void visitCode(const Code *C) {
        print("'");
        print(C->getLiteralContent());
        print("'");
      }

      void visitHTML(const HTML *H) { print(H->getLiteralContent()); }

      void visitInlineHTML(const InlineHTML *IH) {
        print(IH->getLiteralContent());
      }

      void visitSoftBreak(const SoftBreak *SB) { printNewline(); }

      void visitLineBreak(const LineBreak *LB) {
        printNewline();
        printNewline();
      }

      void visitLink(const Link *L) {
        print("[");
        for (const auto *Child : L->getChildren())
          visit(Child);
        print("](");
        print(L->getDestination());
        print(")");
      }

      void visitImage(const Image *I) { llvm_unreachable("unsupported"); }

      void visitParagraph(const Paragraph *P) {
        for (const auto *Child : P->getChildren())
          visit(Child);
        printNewline();
      }

      // TODO: add raw_ostream support for italics ANSI codes in LLVM.
      void visitEmphasis(const Emphasis *E) {
        for (const auto *Child : E->getChildren())
          visit(Child);
      }

      void visitStrong(const Strong *E) {
        bold();
        for (const auto *Child : E->getChildren())
          visit(Child);
        unbold();
      }

      void visitHRule(const HRule *HR) {
        print("--------------");
        printNewline();
      }

      void visitHeader(const Header *H) {
        bold();
        for (const auto *Child : H->getChildren())
          visit(Child);
        unbold();
        printNewline();
      }

      void visitText(const Text *T) { print(T->getLiteralContent()); }

      void visitPrivateExtension(const PrivateExtension *PE) {
        llvm_unreachable("unsupported");
      }

      void visitParamField(const ParamField *PF) {
        llvm_unreachable("unsupported");
      }

      void visitReturnField(const ReturnsField *RF) {
        llvm_unreachable("unsupported");
      }

      void visitThrowField(const ThrowsField *TF) {
        llvm_unreachable("unsupported");
      }

  #define MARKUP_SIMPLE_FIELD(Id, Keyword, XMLKind)                              \
    void visit##Id(const Id *Field) { llvm_unreachable("unsupported"); }
  #include "swift/Markup/SimpleFields.def"
    };

    static void printMarkdown(StringRef Content, raw_ostream &Out,
                              bool UseColor) {
      markup::MarkupContext ctx;
      auto document = markup::parseDocument(ctx, Content);
      if (UseColor) {
        ColoredStream stream{Out};
        TerminalMarkupPrinter printer(stream);
        printer.visit(document);
      } else {
        NoColorStream stream{Out};
        TerminalMarkupPrinter printer(stream);
        printer.visit(document);
      }
    }
} // end anonymous namespace

#if SWIFT_BUILD_SWIFT_SYNTAX
/// Enqueue a diagnostic with ASTGen's diagnostic rendering.
static void enqueueDiagnostic(
    void *queuedDiagnostics, const DiagnosticInfo &info, SourceManager &SM
) {
  llvm::SmallString<256> text;
  {
    llvm::raw_svector_ostream out(text);
    DiagnosticEngine::formatDiagnosticText(out, info.FormatString,
                                           info.FormatArgs);
  }

  BridgedDiagnosticSeverity severity;
  switch (info.Kind) {
  case DiagnosticKind::Error:
    severity = BridgedDiagnosticSeverity::BridgedError;
    break;

  case DiagnosticKind::Warning:
    severity = BridgedDiagnosticSeverity::BridgedWarning;
    break;

  case DiagnosticKind::Remark:
    severity = BridgedDiagnosticSeverity::BridgedRemark;
    break;

  case DiagnosticKind::Note:
    severity = BridgedDiagnosticSeverity::BridgedNote;
    break;
  }

  // Map the highlight ranges.
  SmallVector<const void *, 2> highlightRanges;
  for (const auto &range : info.Ranges) {
    if (range.isInvalid())
      continue;

    highlightRanges.push_back(range.getStart().getOpaquePointerValue());
    highlightRanges.push_back(range.getEnd().getOpaquePointerValue());
  }

  // FIXME: Translate Fix-Its.

  swift_ASTGen_addQueuedDiagnostic(
      queuedDiagnostics, text.data(), text.size(), severity,
      info.Loc.getOpaquePointerValue(),
      highlightRanges.data(), highlightRanges.size() / 2);
}

/// Retrieve the stack of source buffers from the provided location out to
/// a physical source file, with source buffer IDs for each step along the way
/// due to (e.g.) macro expansions or generated code.
///
/// The resulting vector will always contain valid source locations. If the
/// initial location is invalid, the result will be empty.
static SmallVector<unsigned, 1> getSourceBufferStack(
    SourceManager &sourceMgr, SourceLoc loc) {
  SmallVector<unsigned, 1> stack;
  while (true) {
    if (loc.isInvalid())
      return stack;

    unsigned bufferID = sourceMgr.findBufferContainingLoc(loc);
    stack.push_back(bufferID);

    auto generatedSourceInfo = sourceMgr.getGeneratedSourceInfo(bufferID);
    if (!generatedSourceInfo)
      return stack;

    loc = generatedSourceInfo->originalSourceRange.getStart();
  }
}

void *PrintingDiagnosticConsumer::getSourceFileSyntax(
    SourceManager &sourceMgr, unsigned bufferID, StringRef displayName) {
  auto known = sourceFileSyntax.find({&sourceMgr, bufferID});
  if (known != sourceFileSyntax.end())
    return known->second;

  auto bufferContents = sourceMgr.getEntireTextForBuffer(bufferID);
  auto sourceFile = swift_ASTGen_parseSourceFile(
      bufferContents.data(), bufferContents.size(),
      "module", displayName.str().c_str(), /*ctx*/ nullptr);

  sourceFileSyntax[{&sourceMgr, bufferID}] = sourceFile;
  return sourceFile;
}

void PrintingDiagnosticConsumer::queueBuffer(
    SourceManager &sourceMgr, unsigned bufferID) {
  QueuedBuffer knownSourceFile = queuedBuffers[bufferID];
  if (knownSourceFile)
    return;

  // Find the parent and position in parent, if there is one.
  int parentID = -1;
  int positionInParent = 0;
  std::string displayName;
  auto generatedSourceInfo = sourceMgr.getGeneratedSourceInfo(bufferID);
  if (generatedSourceInfo) {
    SourceLoc parentLoc = generatedSourceInfo->originalSourceRange.getEnd();
    if (parentLoc.isValid()) {
      parentID = sourceMgr.findBufferContainingLoc(parentLoc);
      positionInParent = sourceMgr.getLocOffsetInBuffer(parentLoc, parentID);

      // Queue the parent buffer.
      queueBuffer(sourceMgr, parentID);
    }

    if (DeclName macroName =
            getGeneratedSourceInfoMacroName(*generatedSourceInfo)) {
      SmallString<64> buffer;
      if (generatedSourceInfo->attachedMacroCustomAttr)
        displayName = ("macro expansion @" + macroName.getString(buffer)).str();
      else
        displayName = ("macro expansion #" + macroName.getString(buffer)).str();
    }
  }

  if (displayName.empty()) {
    displayName = sourceMgr.getDisplayNameForLoc(
        sourceMgr.getLocForBufferStart(bufferID)).str();
  }

  auto sourceFile = getSourceFileSyntax(sourceMgr, bufferID, displayName);
  swift_ASTGen_addQueuedSourceFile(
      queuedDiagnostics, bufferID, sourceFile,
      (const uint8_t*)displayName.data(), displayName.size(),
      parentID, positionInParent);
  queuedBuffers[bufferID] = sourceFile;
}
#endif // SWIFT_BUILD_SWIFT_SYNTAX

// MARK: Main DiagnosticConsumer entrypoint.
void PrintingDiagnosticConsumer::handleDiagnostic(SourceManager &SM,
                                                  const DiagnosticInfo &Info) {
  if (Info.Kind == DiagnosticKind::Error) {
    DidErrorOccur = true;
  }

  if (SuppressOutput)
    return;

  if (Info.IsChildNote)
    return;

  switch (FormattingStyle) {
  case DiagnosticOptions::FormattingStyle::Swift: {
#if SWIFT_BUILD_SWIFT_SYNTAX
    // Use the swift-syntax formatter.
    auto bufferStack = getSourceBufferStack(SM, Info.Loc);
    if (!bufferStack.empty()) {
      // If there are no enqueued diagnostics, or we have hit a non-note
	// diagnostic, flush any enqueued diagnostics and start fresh.
      if (!queuedDiagnostics || Info.Kind != DiagnosticKind::Note) {
        flush(/*includeTrailingBreak*/ true);
        queuedDiagnostics = swift_ASTGen_createQueuedDiagnostics();
      }

      unsigned innermostBufferID = bufferStack.front();
      queueBuffer(SM, innermostBufferID);
      enqueueDiagnostic(queuedDiagnostics, Info, SM);
      break;
    }
#endif

    // Fall through when we don't have the new diagnostics renderer available.
    LLVM_FALLTHROUGH;
  }

  case DiagnosticOptions::FormattingStyle::LLVM:
    printDiagnostic(SM, Info);

    if (PrintEducationalNotes) {
      for (auto path : Info.EducationalNotePaths) {
        if (auto buffer = SM.getFileSystem()->getBufferForFile(path)) {
          printMarkdown(buffer->get()->getBuffer(), Stream, ForceColors);
          Stream << "\n";
        }
      }
    }

    for (auto ChildInfo : Info.ChildDiagnosticInfo) {
      printDiagnostic(SM, *ChildInfo);
    }
    break;
  }
}

void PrintingDiagnosticConsumer::flush(bool includeTrailingBreak) {
#if SWIFT_BUILD_SWIFT_SYNTAX
  if (queuedDiagnostics) {
    BridgedStringRef bridgedRenderedString{nullptr, 0};
    swift_ASTGen_renderQueuedDiagnostics(queuedDiagnostics, /*contextSize=*/2,
                                         ForceColors ? 1 : 0,
                                         &bridgedRenderedString);
    auto renderedString = bridgedRenderedString.unbridged();
    if (renderedString.data()) {
      Stream.write(renderedString.data(), renderedString.size());
      swift_ASTGen_freeBridgedString(renderedString);
    }
    swift_ASTGen_destroyQueuedDiagnostics(queuedDiagnostics);
    queuedDiagnostics = nullptr;
    queuedBuffers.clear();

    if (includeTrailingBreak)
      Stream << "\n";
  }
#endif

  for (auto note : BufferedEducationalNotes) {
    printMarkdown(note, Stream, ForceColors);
    Stream << "\n";
  }

  BufferedEducationalNotes.clear();
}

bool PrintingDiagnosticConsumer::finishProcessing() {
  // If there's an in-flight snippet, flush it.
  flush(false);
  return false;
}

// MARK: LLVM style diagnostic printing
void PrintingDiagnosticConsumer::printDiagnostic(SourceManager &SM,
                                                 const DiagnosticInfo &Info) {

  // Determine what kind of diagnostic we're emitting.
  llvm::SourceMgr::DiagKind SMKind;
  switch (Info.Kind) {
  case DiagnosticKind::Error:
    SMKind = llvm::SourceMgr::DK_Error;
    break;
  case DiagnosticKind::Warning:
    SMKind = llvm::SourceMgr::DK_Warning;
    break;

  case DiagnosticKind::Note:
    SMKind = llvm::SourceMgr::DK_Note;
    break;

  case DiagnosticKind::Remark:
    SMKind = llvm::SourceMgr::DK_Remark;
    break;
  }

  // Translate ranges.
  SmallVector<llvm::SMRange, 2> Ranges;
  for (auto R : Info.Ranges)
    Ranges.push_back(getRawRange(SM, R));

  // Translate fix-its.
  SmallVector<llvm::SMFixIt, 2> FixIts;
  for (DiagnosticInfo::FixIt F : Info.FixIts)
    FixIts.push_back(getRawFixIt(SM, F));

  // Display the diagnostic.
  ColoredStream coloredErrs{Stream};
  raw_ostream &out = ForceColors ? coloredErrs : Stream;
  const llvm::SourceMgr &rawSM = SM.getLLVMSourceMgr();
  
  // Actually substitute the diagnostic arguments into the diagnostic text.
  llvm::SmallString<256> Text;
  {
    llvm::raw_svector_ostream Out(Text);
    DiagnosticEngine::formatDiagnosticText(Out, Info.FormatString,
                                           Info.FormatArgs);
  }

  auto Msg = SM.GetMessage(Info.Loc, SMKind, Text, Ranges, FixIts,
                           EmitMacroExpansionFiles);
  rawSM.PrintMessage(out, Msg, ForceColors);
}

llvm::SMDiagnostic
SourceManager::GetMessage(SourceLoc Loc, llvm::SourceMgr::DiagKind Kind,
                          const Twine &Msg,
                          ArrayRef<llvm::SMRange> Ranges,
                          ArrayRef<llvm::SMFixIt> FixIts,
                          bool EmitMacroExpansionFiles) const {

  // First thing to do: find the current buffer containing the specified
  // location to pull out the source line.
  SmallVector<std::pair<unsigned, unsigned>, 4> ColRanges;
  std::pair<unsigned, unsigned> LineAndCol;
  StringRef BufferID = "<unknown>";
  std::string LineStr;

  if (Loc.isValid()) {
    BufferID = getDisplayNameForLoc(Loc, EmitMacroExpansionFiles);
    auto CurMB = LLVMSourceMgr.getMemoryBuffer(findBufferContainingLoc(Loc));

    // Scan backward to find the start of the line.
    const char *LineStart = Loc.Value.getPointer();
    const char *BufStart = CurMB->getBufferStart();
    while (LineStart != BufStart && LineStart[-1] != '\n' &&
           LineStart[-1] != '\r')
      --LineStart;

    // Get the end of the line.
    const char *LineEnd = Loc.Value.getPointer();
    const char *BufEnd = CurMB->getBufferEnd();
    while (LineEnd != BufEnd && LineEnd[0] != '\n' && LineEnd[0] != '\r')
      ++LineEnd;
    LineStr = std::string(LineStart, LineEnd);

    // Convert any ranges to column ranges that only intersect the line of the
    // location.
    for (unsigned i = 0, e = Ranges.size(); i != e; ++i) {
      llvm::SMRange R = Ranges[i];
      if (!R.isValid()) continue;

      // If the line doesn't contain any part of the range, then ignore it.
      if (R.Start.getPointer() > LineEnd || R.End.getPointer() < LineStart)
        continue;

      // Ignore pieces of the range that go onto other lines.
      if (R.Start.getPointer() < LineStart)
        R.Start = llvm::SMLoc::getFromPointer(LineStart);
      if (R.End.getPointer() > LineEnd)
        R.End = llvm::SMLoc::getFromPointer(LineEnd);

      // Translate from SMLoc ranges to column ranges.
      // FIXME: Handle multibyte characters.
      ColRanges.push_back(std::make_pair(R.Start.getPointer()-LineStart,
                                         R.End.getPointer()-LineStart));
    }

    LineAndCol = getPresumedLineAndColumnForLoc(Loc);
  }

  return llvm::SMDiagnostic(LLVMSourceMgr, Loc.Value, BufferID,
                            LineAndCol.first,
                            LineAndCol.second-1, Kind, Msg.str(),
                            LineStr, ColRanges, FixIts);
}

// These must come after the declaration of AnnotatedSourceSnippet due to the
// `currentSnippet` member.
PrintingDiagnosticConsumer::PrintingDiagnosticConsumer(
    llvm::raw_ostream &stream)
    : Stream(stream) {}

PrintingDiagnosticConsumer::~PrintingDiagnosticConsumer() {
#if SWIFT_BUILD_SWIFT_SYNTAX
  for (const auto &sourceFileSyntax : sourceFileSyntax) {
    swift_ASTGen_destroySourceFile(sourceFileSyntax.second);
  }
#endif
}
