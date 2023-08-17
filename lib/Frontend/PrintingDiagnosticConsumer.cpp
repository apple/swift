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
#include "swift/AST/CASTBridging.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/DiagnosticsCommon.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Markup/Markup.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/FormatAdapters.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>

using namespace swift;
using namespace swift::markup;

extern "C" void *swift_ASTGen_createQueuedDiagnostics();
extern "C" void swift_ASTGen_destroyQueuedDiagnostics(void *queued);
extern "C" void swift_ASTGen_addQueuedSourceFile(
      void *queuedDiagnostics,
      int bufferID,
      void *sourceFile,
      const uint8_t *displayNamePtr,
      intptr_t displayNameLength,
      int parentID,
      int positionInParent);
extern "C" void swift_ASTGen_addQueuedDiagnostic(
    void *queued,
    const char* text, ptrdiff_t textLength,
    BridgedDiagnosticSeverity severity,
    const void *sourceLoc,
    const void **highlightRanges,
    ptrdiff_t numHighlightRanges
);
extern "C" void swift_ASTGen_renderQueuedDiagnostics(
    void *queued, ptrdiff_t contextSize, ptrdiff_t colorize,
    char **outBuffer, ptrdiff_t *outBufferLength);

// FIXME: Hack because we cannot easily get to the already-parsed source
// file from here. Fix this egregious oversight!
extern "C" void *swift_ASTGen_parseSourceFile(const char *buffer,
                                              size_t bufferLength,
                                              const char *moduleName,
                                              const char *filename,
                                              void *_Nullable ctx);
extern "C" void swift_ASTGen_destroySourceFile(void *sourceFile);

namespace {
  class ColoredStream : public raw_ostream {
    raw_ostream &Underlying;
  public:
    explicit ColoredStream(raw_ostream &underlying) : Underlying(underlying) {}
    ~ColoredStream() override { flush(); }

    raw_ostream &changeColor(Colors color, bool bold = false,
                             bool bg = false) override {
      Underlying.changeColor(color, bold, bg);
      return *this;
    }
    raw_ostream &resetColor() override {
      Underlying.resetColor();
      return *this;
    }
    raw_ostream &reverseColor() override {
      Underlying.reverseColor();
      return *this;
    }
    bool has_colors() const override {
      return true;
    }

    void write_impl(const char *ptr, size_t size) override {
      Underlying.write(ptr, size);
    }
    uint64_t current_pos() const override {
      return Underlying.tell() - GetNumBytesInBuffer();
    }

    size_t preferred_buffer_size() const override {
      return 0;
    }
  };

  /// A stream which drops all color settings.
  class NoColorStream : public raw_ostream {
    raw_ostream &Underlying;

  public:
    explicit NoColorStream(raw_ostream &underlying) : Underlying(underlying) {}
    ~NoColorStream() override { flush(); }

    bool has_colors() const override { return false; }

    void write_impl(const char *ptr, size_t size) override {
      Underlying.write(ptr, size);
    }
    uint64_t current_pos() const override {
      return Underlying.tell() - GetNumBytesInBuffer();
    }

    size_t preferred_buffer_size() const override { return 0; }
  };

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

  // MARK: Experimental diagnostic printing.

  static void printDiagnosticKind(DiagnosticKind kind, raw_ostream &out) {
    switch (kind) {
    case DiagnosticKind::Error:
      out.changeColor(ColoredStream::Colors::RED, true);
      out << "error:";
      break;
    case DiagnosticKind::Warning:
      out.changeColor(ColoredStream::Colors::YELLOW, true);
      out << "warning:";
      break;
    case DiagnosticKind::Note:
      out.changeColor(ColoredStream::Colors::CYAN, true);
      out << "note:";
      break;
    case DiagnosticKind::Remark:
      out.changeColor(ColoredStream::Colors::CYAN, true);
      out << "remark:";
      break;
    }
    out.resetColor();
  }

  static void printNumberedGutter(unsigned LineNumber,
                                  unsigned LineNumberIndent, raw_ostream &Out) {
    Out.changeColor(ColoredStream::Colors::CYAN);
    Out << llvm::formatv(
        "{0} | ",
        llvm::fmt_align(LineNumber, llvm::AlignStyle::Right, LineNumberIndent));
    Out.resetColor();
  }

  static void printEmptyGutter(unsigned LineNumberIndent, raw_ostream &Out) {
    Out.changeColor(ColoredStream::Colors::CYAN);
    Out << std::string(LineNumberIndent + 1, ' ') << "| ";
    Out.resetColor();
  }

  static void printStringAsSingleQuotedLine(StringRef str, raw_ostream &Out) {
    Out << "'";
    for (auto character : str) {
      if (character == '\n')
        Out << "\\n";
      else
        Out << character;
    }
    Out << "'";
  }

  // Describe a fix-it out-of-line.
  static void describeFixIt(SourceManager &SM, DiagnosticInfo::FixIt fixIt,
                            raw_ostream &Out) {
    if (fixIt.getRange().getByteLength() == 0) {
      Out << "insert ";
      printStringAsSingleQuotedLine(fixIt.getText(), Out);
    } else if (fixIt.getText().empty()) {
      Out << "remove ";
      printStringAsSingleQuotedLine(SM.extractText(fixIt.getRange()), Out);
    } else {
      Out << "replace ";
      printStringAsSingleQuotedLine(SM.extractText(fixIt.getRange()), Out);
      Out << " with ";
      printStringAsSingleQuotedLine(fixIt.getText(), Out);
    }
  }

  static void describeFixIts(SourceManager &SM,
                             ArrayRef<DiagnosticInfo::FixIt> fixIts,
                             raw_ostream &Out) {
    Out << "[";
    for (unsigned i = 0; i < fixIts.size(); ++i) {
      if (fixIts.size() > 2 && i + 1 == fixIts.size()) {
        Out << ", and ";
      } else if (fixIts.size() > 2 && i > 0) {
        Out << ", ";
      } else if (fixIts.size() == 2 && i == 1) {
        Out << " and ";
      }
      describeFixIt(SM, fixIts[i], Out);
    }
    Out << "]";
  }

  /// Represents a single line of source code annotated with optional messages,
  /// highlights, and fix-its.
  class AnnotatedLine {
    friend class AnnotatedFileExcerpt;

    // A diagnostic message located at a specific byte in the line.
    struct Message {
      unsigned Byte;
      DiagnosticKind Kind;
      std::string Text;
    };

    // A half-open byte range which should be highlighted.
    struct Highlight {
      unsigned StartByte;
      unsigned EndByte;
    };

    // A half-open byte range which should be replaced with the given text.
    struct FixIt {
      unsigned StartByte;
      unsigned EndByte;
      std::string Text;
    };

    unsigned LineNumber;
    // The line number displayed to the user. This may differ from the actual
    // line number if #sourceLocation is used.
    unsigned DisplayLineNumber;
    std::string LineText;
    SmallVector<Message, 1> Messages;
    SmallVector<Highlight, 1> Highlights;
    SmallVector<FixIt, 1> FixIts;

    // Adjust output color as needed if this byte is part of a fix-it deletion.
    void applyStyleForLineByte(unsigned Byte, raw_ostream &Out, bool &Deleted) {
      bool shouldDelete = false;

      for (auto fixIt : FixIts) {
        if (Byte >= fixIt.StartByte && Byte < fixIt.EndByte)
          shouldDelete = true;
      }

      // Only modify deletions when we reach the start or end of
      // a fix-it. This ensures that so long as the original
      // SourceLocs pointed to the first byte of a grapheme cluster, we won't
      // output an ANSI escape sequence in the middle of one.
      if (shouldDelete != Deleted) {
        Out.resetColor();
        if (shouldDelete) {
          Out.changeColor(ColoredStream::Colors::RED, /*bold*/ true);
        }
      }
      Deleted = shouldDelete;
    }

    // Insert fix-it replacement text at the appropriate point in the line.
    bool maybePrintInsertionAfter(int Byte, bool isLineASCII,
                                  raw_ostream &Out) {
      // Don't print insertions inline for non-ASCII lines, because we can't
      // print an underline beneath them.
      if (!isLineASCII)
        return false;

      for (auto fixIt : FixIts) {
        if ((int)fixIt.EndByte - 1 == Byte) {
          Out.changeColor(ColoredStream::Colors::GREEN, /*bold*/ true);
          for (unsigned i = 0; i < fixIt.Text.size(); ++i) {
            // Invert text colors for editor placeholders.
            if (i + 1 < fixIt.Text.size() && fixIt.Text.substr(i, 2) == "<#") {
              Out.changeColor(ColoredStream::Colors::GREEN, /*bold*/ true,
                              /*background*/ true);
              ++i;
            } else if (i + 1 < fixIt.Text.size() &&
                       fixIt.Text.substr(i, 2) == "#>") {
              Out.changeColor(ColoredStream::Colors::GREEN, /*bold*/ true,
                              /*background*/ false);
              ++i;
            } else {
              Out << fixIt.Text[i];
            }
          }
          Out.resetColor();
          return true;
        }
      }
      return false;
    }

    unsigned lineByteOffsetForLoc(SourceManager &SM, SourceLoc Loc) {
      SourceLoc lineStart = SM.getLocForLineCol(SM.findBufferContainingLoc(Loc),
                                                getLineNumber(), 1);
      return SM.getByteDistance(lineStart, Loc);
    }

  public:
    AnnotatedLine(unsigned LineNumber, unsigned DisplayLineNumber,
                  StringRef LineText)
        : LineNumber(LineNumber), DisplayLineNumber(DisplayLineNumber),
          LineText(LineText) {}

    unsigned getLineNumber() { return LineNumber; }

    void addMessage(SourceManager &SM, SourceLoc Loc, DiagnosticKind Kind,
                    StringRef Message) {
      Messages.push_back({lineByteOffsetForLoc(SM, Loc), Kind, Message.str()});
    }

    void addHighlight(SourceManager &SM, CharSourceRange Range) {
      Highlights.push_back({lineByteOffsetForLoc(SM, Range.getStart()),
                            lineByteOffsetForLoc(SM, Range.getEnd())});
    }

    void addFixIt(SourceManager &SM, CharSourceRange Range, StringRef Text) {
      FixIts.push_back({lineByteOffsetForLoc(SM, Range.getStart()),
                        lineByteOffsetForLoc(SM, Range.getEnd()), Text.str()});
    }

    void render(unsigned LineNumberIndent, raw_ostream &Out) {
      printNumberedGutter(DisplayLineNumber, LineNumberIndent, Out);

      // Determine if the line is all-ASCII. This will determine a number of
      // later formatting decisions.
      bool isASCII = true;
      for (unsigned i = 0; i < LineText.size(); ++i)
        isASCII = isASCII && static_cast<unsigned char>(LineText[i]) <= 127;

      // Map a byte in the original source line to a column in the annotated
      // line.
      unsigned *byteToColumnMap = new unsigned[LineText.size() + 1];
      unsigned extraColumns = 0;
      // Track the location of the first character in the line that is not a
      // whitespace character. This can be used to avoid underlining leading
      // whitespace, which looks weird even though it is technically accurate.
      unsigned firstNonWhitespaceColumn = 0;
      bool seenNonWhitespaceCharacter = false;
      // We count one past the end of LineText here to handle trailing fix-it
      // insertions.
      for (unsigned i = 0; i < LineText.size() + 1; ++i) {
        if (isASCII) {
          for (auto fixIt : FixIts) {
            if (fixIt.EndByte == i) {
              // We don't print editor placeholder indicators, so make sure we
              // don't count them here.
              extraColumns += fixIt.Text.size() -
                              StringRef(fixIt.Text).count("<#") * 2 -
                              StringRef(fixIt.Text).count("#>") * 2;
            }
          }
        }

        if (i < LineText.size()) {
          // Tabs are mapped to 2 spaces so they have a known column width.
          if (LineText[i] == '\t')
            extraColumns += 1;

          if (!seenNonWhitespaceCharacter && !isspace(LineText[i])) {
            firstNonWhitespaceColumn = i + extraColumns;
            seenNonWhitespaceCharacter = true;
          }
        }

        byteToColumnMap[i] = i + extraColumns;
      }

      // Print the source line byte-by-byte, emitting ANSI escape sequences as
      // needed to style fix-its, and checking for non-ASCII characters.
      bool deleted = false;
      maybePrintInsertionAfter(-1, isASCII, Out);
      for (unsigned i = 0; i < LineText.size(); ++i) {
        applyStyleForLineByte(i, Out, deleted);
        if (LineText[i] == '\t')
          Out << "  ";
        else
          Out << LineText[i];
        if (maybePrintInsertionAfter(i, isASCII, Out)) {
          deleted = false;
        }
      }
      maybePrintInsertionAfter(LineText.size(), isASCII, Out);
      Out.resetColor();
      Out << "\n";

      // If the entire line is composed of ASCII characters, we can position '~'
      // characters in the appropriate columns on the following line to
      // represent highlights.
      if (isASCII) {
        auto highlightLine = std::string(byteToColumnMap[LineText.size()], ' ');
        for (auto highlight : Highlights) {
          for (unsigned i =
                   std::max(highlight.StartByte, firstNonWhitespaceColumn);
               i < highlight.EndByte; ++i)
            highlightLine[byteToColumnMap[i]] = '~';
        }

        for (auto fixIt : FixIts) {
          // Mark deletions.
          for (unsigned i = std::max(fixIt.StartByte, firstNonWhitespaceColumn);
               i < fixIt.EndByte; ++i)
            highlightLine[byteToColumnMap[i]] = '-';
          // Mark insertions. If the fix-it starts at the beginning of the line,
          // highlight from column zero to the end column. Otherwise, find the
          // column which immediately precedes the insertion. Then, highlight
          // from the column after that to the end column. The end column in
          // this case is obtained from the fix-it's starting byte, because
          // insertions are printed before the deleted range.
          unsigned startColumn = fixIt.StartByte == 0
                                     ? 0
                                     : byteToColumnMap[fixIt.StartByte - 1] + 1;
          for (unsigned i = startColumn; i < byteToColumnMap[fixIt.StartByte];
               ++i)
            highlightLine[i] = '+';
        }

        // Print the highlight line with the appropriate colors.
        if (!(Highlights.empty() && FixIts.empty())) {
          printEmptyGutter(LineNumberIndent, Out);
          auto currentColor = ColoredStream::Colors::WHITE;
          for (unsigned i = 0; i < highlightLine.size(); ++i) {
            llvm::raw_ostream::Colors charColor;
            switch (highlightLine[i]) {
            case '+':
              charColor = ColoredStream::Colors::GREEN;
              break;
            case '-':
              charColor = ColoredStream::Colors::RED;
              break;
            case '~':
              charColor = ColoredStream::Colors::BLUE;
              break;
            default:
              charColor = ColoredStream::Colors::WHITE;
              break;
            }
            if (currentColor != charColor) {
              currentColor = charColor;
              Out.changeColor(charColor, /*bold*/ true);
            }
            Out << highlightLine[i];
          }
          Out.resetColor();
          Out << "\n";
        }
      }

      // Print each message on its own line below the source line. If the source
      // line is ASCII, we can insert a caret pointing directly to the message
      // location. If not, use a more generic "-->" indicator.
      // FIXME: Improve Unicode support so every message can include a direct
      // location indicator.
      for (auto msg : Messages) {
        printEmptyGutter(LineNumberIndent, Out);
        if (isASCII) {
          Out << std::string(byteToColumnMap[msg.Byte], ' ') << "^ ";
        } else {
          Out.changeColor(ColoredStream::Colors::CYAN);
          Out << "--> ";
          Out.resetColor();
        }
        printDiagnosticKind(msg.Kind, Out);
        Out.resetColor();
        Out.changeColor(ColoredStream::Colors::WHITE, /*bold*/ true);
        Out << " " << msg.Text << "\n";
        Out.resetColor();
      }
      delete[] byteToColumnMap;
    }
  };

  /// Represents an excerpt of a source file which contains one or more
  /// annotated source lines.
  class AnnotatedFileExcerpt {
    SourceManager &SM;
    unsigned BufferID;
    /// The primary location of the parent error/warning/remark for this
    /// diagnostic message. This is printed alongside the file path so it can be
    /// parsed by editors and other tooling.
    SourceLoc PrimaryLoc;
    /// Whether the excerpt is from a virtual file (e.g. one introduced using
    /// #sourceLocation).
    bool FromVirtualFile;
    std::vector<AnnotatedLine> AnnotatedLines;

    /// Return the AnnotatedLine for a given SourceLoc, creating it if it
    /// doesn't already exist.
    AnnotatedLine &lineForLoc(SourceLoc Loc) {
      // FIXME: This call to `getLineNumber` is expensive.
      unsigned lineNo = SM.getLineAndColumnInBuffer(Loc).first;
      AnnotatedLine newLine(lineNo, 0, "");
      auto iter =
          std::lower_bound(AnnotatedLines.begin(), AnnotatedLines.end(),
                           newLine, [](AnnotatedLine l1, AnnotatedLine l2) {
                             return l1.getLineNumber() < l2.getLineNumber();
                           });
      if (iter == AnnotatedLines.end() || iter->getLineNumber() != lineNo) {
        newLine.LineText = SM.getLineString(BufferID, lineNo);
        newLine.DisplayLineNumber =
            SM.getPresumedLineAndColumnForLoc(Loc).first;
        return *AnnotatedLines.insert(iter, newLine);
      } else {
        return *iter;
      }
    }

    void printNumberedLine(SourceManager &SM, unsigned BufferID,
                           unsigned LineNumber, unsigned LineNumberIndent,
                           raw_ostream &Out) {
      printNumberedGutter(LineNumber, LineNumberIndent, Out);
      Out << SM.getLineString(BufferID, LineNumber) << "\n";
    }

    void lineRangesForRange(CharSourceRange Range,
                            SmallVectorImpl<CharSourceRange> &LineRanges) {
      unsigned startLineNo =
          SM.getLineAndColumnInBuffer(Range.getStart()).first;
      unsigned endLineNo = SM.getLineAndColumnInBuffer(Range.getEnd()).first;

      if (startLineNo == endLineNo) {
        LineRanges.push_back(Range);
        return;
      }

      // Split the range by line.
      SourceLoc lineEnd = SM.getLocForOffset(
          BufferID, *SM.resolveOffsetForEndOfLine(BufferID, startLineNo));
      LineRanges.push_back(CharSourceRange(SM, Range.getStart(), lineEnd));

      for (unsigned intermediateLine = startLineNo + 1;
           intermediateLine < endLineNo; ++intermediateLine) {
        SourceLoc lineStart =
            SM.getLocForLineCol(BufferID, intermediateLine, 1);
        SourceLoc lineEnd = SM.getLocForOffset(
            BufferID,
            *SM.resolveOffsetForEndOfLine(BufferID, intermediateLine));
        LineRanges.push_back(CharSourceRange(SM, lineStart, lineEnd));
      }

      SourceLoc lastLineStart = SM.getLocForLineCol(BufferID, endLineNo, 1);
      LineRanges.push_back(CharSourceRange(SM, lastLineStart, Range.getEnd()));
    }

    void printLineEllipsis(raw_ostream &Out) {
      Out.changeColor(ColoredStream::Colors::CYAN, true);
      Out << llvm::formatv("{0}...\n",
                           llvm::fmt_repeat(" ", getPreferredLineNumberIndent()));
      Out.resetColor();
    }

  public:
    AnnotatedFileExcerpt(SourceManager &SM, unsigned BufferID,
                         SourceLoc PrimaryLoc)
        : SM(SM), BufferID(BufferID), PrimaryLoc(PrimaryLoc) {
      FromVirtualFile = SM.isLocInVirtualFile(PrimaryLoc);
    }

    unsigned getPreferredLineNumberIndent() {
      // The lines are already in sorted ascending order, and we render one line
      // after the last one for context. Use the last line number plus one to
      // determine the indent.
      return floor(1 + log10(AnnotatedLines.back().getLineNumber() + 1));
    }

    void addMessage(SourceLoc Loc, DiagnosticKind Kind, StringRef Message) {
      lineForLoc(Loc).addMessage(SM, Loc, Kind, Message);
    }

    void addHighlight(CharSourceRange Range) {
      SmallVector<CharSourceRange, 1> ranges;
      lineRangesForRange(Range, ranges);
      for (auto lineRange : ranges)
        lineForLoc(lineRange.getStart()).addHighlight(SM, lineRange);
    }

    void addFixIt(CharSourceRange Range, StringRef Text) {
      SmallVector<CharSourceRange, 1> ranges;
      lineRangesForRange(Range, ranges);
      // The removals are broken down line-by-line, so only add any insertions
      // to the last replacement.
      auto last = ranges.pop_back_val();
      lineForLoc(last.getStart()).addFixIt(SM, last, Text);
      for (auto lineRange : ranges)
        lineForLoc(lineRange.getStart()).addFixIt(SM, lineRange, "");
    }

    void render(unsigned MinimumLineNumberIndent, raw_ostream &Out) {
      // Tha maximum number of intermediate lines without annotations to render
      // between annotated lines before using an ellipsis.
      static const unsigned maxIntermediateLines = 3;

      assert(!AnnotatedLines.empty() && "File excerpt has no lines");
      unsigned lineNumberIndent =
          std::max(getPreferredLineNumberIndent(), MinimumLineNumberIndent);

      // Print the file name at the top of each excerpt.
      auto primaryLineAndColumn = SM.getPresumedLineAndColumnForLoc(PrimaryLoc);
      Out.changeColor(ColoredStream::Colors::CYAN);
      Out << std::string(lineNumberIndent + 1, '=') << " "
          << SM.getDisplayNameForLoc(PrimaryLoc) << ":"
          << primaryLineAndColumn.first << ":" << primaryLineAndColumn.second
          << " " << std::string(lineNumberIndent + 1, '=') << "\n";
      Out.resetColor();

      // Print one extra line at the top for context, so long as this isn't an
      // excerpt from a virtual file.
      if (AnnotatedLines.front().getLineNumber() > 1 && !FromVirtualFile)
        printNumberedLine(SM, BufferID,
                          AnnotatedLines.front().getLineNumber() - 1,
                          lineNumberIndent, Out);

      // Render the first annotated line.
      AnnotatedLines.front().render(lineNumberIndent, Out);
      unsigned lastLineNumber = AnnotatedLines.front().getLineNumber();

      // Render intermediate lines/ellipsis, followed by the next annotated
      // line until they have all been output.
      for (auto line = AnnotatedLines.begin() + 1; line != AnnotatedLines.end();
           ++line) {
        unsigned lineNumber = line->getLineNumber();
        if (FromVirtualFile) {
          // Don't print intermediate lines in virtual files, as they may not
          // make sense in context. Instead, just print an ellipsis between them
          // if they're not consecutive in the actual source file.
          if (lineNumber - lastLineNumber > 1) {
            printLineEllipsis(Out);
          }
        } else if (lineNumber - lastLineNumber > maxIntermediateLines) {
          // Use an ellipsis to denote an omitted part of the file.
          printNumberedLine(SM, BufferID, lastLineNumber + 1, lineNumberIndent,
                            Out);
          printLineEllipsis(Out);
          printNumberedLine(SM, BufferID, lineNumber - 1, lineNumberIndent,
                            Out);
        } else {
          // Print all the intermediate lines.
          for (unsigned l = lastLineNumber + 1; l < lineNumber; ++l) {
            printNumberedLine(SM, BufferID, l, lineNumberIndent, Out);
          }
        }
        // Print the annotated line.
        line->render(lineNumberIndent, Out);
        lastLineNumber = lineNumber;
      }
      // Print one extra line at the bottom for context, so long as the excerpt
      // isn't from a virtual file.
      if (!FromVirtualFile) {
        printNumberedLine(
            SM, BufferID,
            AnnotatedLines[AnnotatedLines.size() - 1].getLineNumber() + 1,
            lineNumberIndent, Out);
      }
    }
  };
} // end anonymous namespace

namespace swift {
/// Represents one or more annotated file snippets which together form a
/// complete diagnostic message.
class AnnotatedSourceSnippet {
  SourceManager &SM;
  std::map<StringRef, AnnotatedFileExcerpt> FileExcerpts;
  SmallVector<std::pair<DiagnosticKind, std::string>, 1>
      UnknownLocationMessages;

  AnnotatedFileExcerpt &excerptForLoc(SourceLoc Loc) {
    StringRef bufName = SM.getDisplayNameForLoc(Loc);
    unsigned bufID = SM.findBufferContainingLoc(Loc);
    // Use the buffer display name as the key in the excerpt map instead of the
    // buffer identifier to respect #sourceLocation directives.
    FileExcerpts.emplace(bufName, AnnotatedFileExcerpt(SM, bufID, Loc));
    return FileExcerpts.find(bufName)->second;
  }

public:
  AnnotatedSourceSnippet(SourceManager &SM) : SM(SM){};

  void addMessage(SourceLoc Loc, DiagnosticKind Kind, StringRef Message) {
    if (Loc.isInvalid()) {
      UnknownLocationMessages.push_back({Kind, Message.str()});
      return;
    }
    excerptForLoc(Loc).addMessage(Loc, Kind, Message);
  }

  void addHighlight(CharSourceRange Range) {
    if (Range.isInvalid())
      return;
    excerptForLoc(Range.getStart()).addHighlight(Range);
  }

  void addFixIt(CharSourceRange Range, StringRef Text) {
    if (Range.isInvalid())
      return;
    excerptForLoc(Range.getStart()).addFixIt(Range, Text);
  }

  void render(raw_ostream &Out) {
    // Print the excerpt for each file.
    unsigned lineNumberIndent = 0;
    if (!FileExcerpts.empty()) {
      lineNumberIndent = std::max_element(FileExcerpts.begin(), FileExcerpts.end(),
          [](auto &a, auto &b) {
            return a.second.getPreferredLineNumberIndent() <
              b.second.getPreferredLineNumberIndent();
          })->second.getPreferredLineNumberIndent();
    }
    for (auto excerpt : FileExcerpts)
      excerpt.second.render(lineNumberIndent, Out);

    // Handle messages with invalid locations.
    if (UnknownLocationMessages.size() == 1) {
      Out.changeColor(ColoredStream::Colors::CYAN);
      Out << "Unknown Location: ";
      Out.resetColor();
      printDiagnosticKind(UnknownLocationMessages[0].first, Out);
      Out << " " << UnknownLocationMessages[0].second << "\n";
    } else if (UnknownLocationMessages.size() > 1) {
      Out.changeColor(ColoredStream::Colors::CYAN);
      Out << "Unknown Location\n";
      Out.resetColor();
      for (auto unknownMessage : UnknownLocationMessages) {
        printEmptyGutter(2, Out);
        printDiagnosticKind(unknownMessage.first, Out);
        Out << " " << unknownMessage.second << "\n";
      }
    }
  }
};
} // namespace swift

static void annotateSnippetWithInfo(SourceManager &SM,
                                    const DiagnosticInfo &Info,
                                    AnnotatedSourceSnippet &Snippet) {
  llvm::SmallString<256> Text;
  {
    llvm::raw_svector_ostream Out(Text);
    DiagnosticEngine::formatDiagnosticText(Out, Info.FormatString,
                                           Info.FormatArgs);
    // Show associated fix-its as part of the message. This is a
    // better experience when notes offer a choice of fix-its. It's redundant
    // for fix-its which are also displayed inline, but helps improve
    // readability in some situations.
    if (!Info.FixIts.empty()) {
      Out << " ";
      describeFixIts(SM, Info.FixIts, Out);
    }
  }

  Snippet.addMessage(Info.Loc, Info.Kind, Text);
  for (auto range : Info.Ranges) {
    Snippet.addHighlight(range);
  }

  // Don't print inline fix-its for notes.
  if (Info.Kind != DiagnosticKind::Note) {
    for (auto fixIt : Info.FixIts) {
      // Don't print multi-line fix-its inline, only include them at the end of
      // the message.
      if (!fixIt.getText().contains("\n"))
        Snippet.addFixIt(fixIt.getRange(), fixIt.getText());
    }
  }
  // Add any explicitly grouped notes to the snippet.
  for (auto ChildInfo : Info.ChildDiagnosticInfo) {
    annotateSnippetWithInfo(SM, *ChildInfo, Snippet);
  }
}

#if SWIFT_SWIFT_PARSER
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
#endif

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

#if SWIFT_SWIFT_PARSER
void PrintingDiagnosticConsumer::queueBuffer(
    SourceManager &sourceMgr, unsigned bufferID) {
  QueuedBuffer knownSourceFile = queuedBuffers[bufferID];
  if (knownSourceFile)
    return;

  auto bufferContents = sourceMgr.getEntireTextForBuffer(bufferID);
  auto sourceFile = swift_ASTGen_parseSourceFile(
      bufferContents.data(), bufferContents.size(),
      "module", "file.swift", /*ctx*/ nullptr);

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

  swift_ASTGen_addQueuedSourceFile(
      queuedDiagnostics, bufferID, sourceFile,
      (const uint8_t*)displayName.data(), displayName.size(),
      parentID, positionInParent);
  queuedBuffers[bufferID] = sourceFile;
}
#endif

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
#if SWIFT_SWIFT_PARSER
    // Use the swift-syntax formatter.
    auto bufferStack = getSourceBufferStack(SM, Info.Loc);
    if (!bufferStack.empty()) {
      // If there are no enqueued diagnostics, or they are from a different
      // outermost buffer, flush any enqueued diagnostics and start fresh.
      unsigned outermostBufferID = bufferStack.back();
      if (!queuedDiagnostics ||
          outermostBufferID != queuedDiagnosticsOutermostBufferID) {
        flush(/*includeTrailingBreak*/ true);

        queuedDiagnosticsOutermostBufferID = outermostBufferID;
        queuedDiagnostics = swift_ASTGen_createQueuedDiagnostics();
      }

      unsigned innermostBufferID = bufferStack.front();
      queueBuffer(SM, innermostBufferID);
      enqueueDiagnostic(queuedDiagnostics, Info, SM);
      break;
    }
#endif

    // Use the C++ formatter.
    // FIXME: Once the swift-syntax formatter is enabled everywhere, we will
    // remove this.
    if (Info.Loc.isValid()) {
      if (Info.Kind == DiagnosticKind::Note && currentSnippet) {
        // If this is a note and we have an in-flight message, add it to that
        // instead of emitting it separately.
        annotateSnippetWithInfo(SM, Info, *currentSnippet);
      } else {
        // If we encounter a new error/warning/remark, flush any in-flight
        // snippets.
        flush(/*includeTrailingBreak*/ true);
        currentSnippet = std::make_unique<AnnotatedSourceSnippet>(SM);
        annotateSnippetWithInfo(SM, Info, *currentSnippet);
      }
      if (PrintEducationalNotes) {
        for (auto path : Info.EducationalNotePaths) {
          if (auto buffer = SM.getFileSystem()->getBufferForFile(path))
            BufferedEducationalNotes.push_back(buffer->get()->getBuffer().str());
        }
      }
      break;
    }

    // Fall through to print using the LLVM style when there is no source
    // location.
    flush(/*includeTrailingBreak*/ false);
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
  if (currentSnippet) {
    if (ForceColors) {
      ColoredStream colorStream{Stream};
      currentSnippet->render(colorStream);
      if (includeTrailingBreak)
        colorStream << "\n";
    } else {
      NoColorStream noColorStream{Stream};
      currentSnippet->render(noColorStream);
      if (includeTrailingBreak)
        noColorStream << "\n";
    }
    currentSnippet.reset();
  }

#if SWIFT_SWIFT_PARSER
  if (queuedDiagnostics) {
    char *renderedString = nullptr;
    ptrdiff_t renderedStringLen = 0;
    swift_ASTGen_renderQueuedDiagnostics(
        queuedDiagnostics, /*contextSize=*/2, ForceColors ? 1 : 0,
        &renderedString, &renderedStringLen);
    if (renderedString) {
      Stream.write(renderedString, renderedStringLen);
    }
    swift_ASTGen_destroyQueuedDiagnostics(queuedDiagnostics);
    queuedDiagnostics = nullptr;
    for (const auto &buffer : queuedBuffers) {
      swift_ASTGen_destroySourceFile(buffer.second);
    }
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
PrintingDiagnosticConsumer::~PrintingDiagnosticConsumer() = default;

// FIXME: This implementation is inefficient.
std::string SourceManager::getLineString(unsigned BufferID,
                                         unsigned LineNumber) {
  SourceLoc Loc = getLocForLineCol(BufferID, LineNumber, 1);
  if (Loc.isInvalid())
    return "";

  auto CurMB = LLVMSourceMgr.getMemoryBuffer(findBufferContainingLoc(Loc));
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
  return std::string(LineStart, LineEnd);
}
