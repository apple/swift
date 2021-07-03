//===--- DiagnosticVerifier.cpp - Diagnostic Verifier (-verify) -----------===//
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
//  This file implements the DiagnosticVerifier class.
//
//===----------------------------------------------------------------------===//

#include "swift/Frontend/DiagnosticVerifier.h"
#include "swift/Basic/STLExtras.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Parse/Lexer.h"
#include "llvm/ADT/FunctionExtras.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

using namespace swift;

static llvm::cl::opt<bool> AbortOnDiagnosticNotBeingProduced(
    "swift-diagnostic-verifier-abort-on-not-produced-diagnostic",
    llvm::cl::Hidden, llvm::cl::init(false),
    llvm::cl::desc(
        "Option for compiler developers to cause the diagnostic verifier to "
        "abort when it detects that it did not find a diagnostic"));

static llvm::cl::opt<bool> AbortOnMalformedDiagnostic(
    "swift-diagnostic-verifier-abort-on-malformed-diagnostic", llvm::cl::Hidden,
    llvm::cl::init(false),
    llvm::cl::desc(
        "Option for compiler developers that causes the diagnostic verifier to "
        "abort when it detects a diagnostic that is malformed structurally"));

static llvm::cl::opt<bool> AbortOnIncorrectDiagnostic(
    "swift-diagnostic-verifier-abort-on-incorrect-diagnostic", llvm::cl::Hidden,
    llvm::cl::init(false),
    llvm::cl::desc("Option for compiler developers to cause the diagnostic "
                   "verifier to abort if it detects a diagnostic in the "
                   "correct location, but with the wrong text."));

static llvm::cl::opt<bool> AbortOnUnexpectedDiagnostic(
    "swift-diagnostic-verifier-abort-on-unexpected-diagnostic",
    llvm::cl::Hidden, llvm::cl::init(false),
    llvm::cl::desc(
        "Option for compiler developers to cause the diagnostic verifier to "
        "abort when it detects a diagnostic that was not specified as "
        "expected"));

static llvm::cl::opt<bool> AbortOnUnexpectedDiagnosticFixit(
    "swift-diagnostic-verifier-abort-on-unexpected-diagnostic-fixit",
    llvm::cl::Hidden, llvm::cl::init(false),
    llvm::cl::desc(
        "Option for compiler developers to cause the diagnostic verifier to "
        "abort when it detects a diagnostic fixit that was not specified as "
        "expected"));

static llvm::cl::opt<bool> AbortOnMissingDiagnostic(
    "swift-diagnostic-verifier-abort-on-missing-diagnostic", llvm::cl::Hidden,
    llvm::cl::init(false),
    llvm::cl::desc(
        "Option for compiler developers to cause the diagnostic verifier to "
        "abort when it detects an expected diagnostic that was missing."));

namespace swift {

struct ExpectedFixIt {
  const char *StartLoc, *EndLoc; // The loc of the {{ and }}'s.
  unsigned StartCol;
  unsigned EndCol;
  std::string Text;
};

} // end namespace swift

namespace {

static constexpr StringLiteral fixitExpectationNoneString("none");
static constexpr StringLiteral educationalNotesSpecifier("educational-notes=");

} // anonymous namespace

struct DiagnosticVerifier::ExpectedDiagnosticInfo {
  // This specifies the full range of the "expected-foo {{}}" specifier.
  const char *ExpectedStart, *ExpectedEnd = nullptr;

  DiagnosticKind Classification;

  // This is true if a '*' constraint is present to say that the diagnostic
  // may appear (or not) an uncounted number of times.
  bool mayAppear = false;

  // This is true if a '{{none}}' is present to mark that there should be no
  // extra fixits.
  bool noExtraFixitsMayAppear() const { return noneMarkerStartLoc != nullptr; };

  // This is the raw input buffer for the message text, the part in the
  // {{...}}
  StringRef MessageRange;

  // This is the message string with escapes expanded.
  std::string MessageStr;
  unsigned LineNo = ~0U;
  Optional<unsigned> ColumnNo;

  std::vector<ExpectedFixIt> Fixits;

  // Loc of {{none}}
  const char *noneMarkerStartLoc = nullptr;

  /// Represents a specifier of the form '{{educational-notes=note1,note2}}'.
  struct ExpectedEducationalNotes {
    const char *StartLoc, *EndLoc; // The loc of the {{ and }}'s.
    llvm::SmallVector<StringRef, 1> Names; // Names of expected notes.

    ExpectedEducationalNotes(const char *StartLoc, const char *EndLoc,
                             llvm::SmallVector<StringRef, 1> Names)
        : StartLoc(StartLoc), EndLoc(EndLoc), Names(Names) {}
  };
  Optional<ExpectedEducationalNotes> EducationalNotes;

  ExpectedDiagnosticInfo(const char *ExpectedStart,
                         DiagnosticKind Classification)
      : ExpectedStart(ExpectedStart), Classification(Classification) {}
};
namespace {
using ExpectedDiagnosticInfo = DiagnosticVerifier::ExpectedDiagnosticInfo;
} // anonymous namespace

static std::string getDiagKindString(DiagnosticKind Kind) {
  switch (Kind) {
  case DiagnosticKind::Error:
    return "error";
  case DiagnosticKind::Warning:
    return "warning";
  case DiagnosticKind::Note:
    return "note";
  case DiagnosticKind::Remark:
    return "remark";
  }

  llvm_unreachable("Unhandled DiagKind in switch.");
}

/// Render the verifier syntax for a given set of educational notes.
static std::string
renderEducationalNotes(llvm::SmallVectorImpl<std::string> &EducationalNotes) {
  std::string Result;
  llvm::raw_string_ostream OS(Result);
  OS << "{{" << educationalNotesSpecifier;
  interleave(EducationalNotes, [&](const auto &Note) { OS << Note; },
             [&] { OS << ','; });
  OS << "}}";
  return OS.str();
}

/// If we find the specified diagnostic in the list, return it.
/// Otherwise return CapturedDiagnostics.end().
static std::vector<CapturedDiagnosticInfo>::iterator
findDiagnostic(std::vector<CapturedDiagnosticInfo> &CapturedDiagnostics,
               const ExpectedDiagnosticInfo &Expected, StringRef BufferName) {
  for (auto I = CapturedDiagnostics.begin(), E = CapturedDiagnostics.end();
       I != E; ++I) {
    // Verify the file and line of the diagnostic.
    if (I->Line != Expected.LineNo || I->FileName != BufferName)
      continue;

    // If a specific column was expected, verify it.
    if (Expected.ColumnNo.hasValue() && I->Column != *Expected.ColumnNo)
      continue;

    // Verify the classification and string.
    if (I->Classification != Expected.Classification ||
        I->Message.find(Expected.MessageStr) == StringRef::npos)
      continue;

    // Okay, we found a match, hurray!
    return I;
  }

  return CapturedDiagnostics.end();
}

/// If there are any -verify errors (e.g. differences between expectations
/// and actual diagnostics produced), apply fixits to the original source
/// file and drop it back in place.
static void autoApplyFixes(SourceManager &SM, unsigned BufferID,
                           ArrayRef<llvm::SMDiagnostic> diags) {
  // Walk the list of diagnostics, pulling out any fixits into an array of just
  // them.
  SmallVector<llvm::SMFixIt, 4> FixIts;
  for (auto &diag : diags)
    FixIts.append(diag.getFixIts().begin(), diag.getFixIts().end());

  // If we have no fixits to apply, avoid touching the file.
  if (FixIts.empty())
    return;

  // Sort the fixits by their start location.
  std::sort(FixIts.begin(), FixIts.end(),
            [&](const llvm::SMFixIt &lhs, const llvm::SMFixIt &rhs) -> bool {
              return lhs.getRange().Start.getPointer() <
                     rhs.getRange().Start.getPointer();
            });
  // Coalesce identical fix-its. This happens most often with "expected-error 2"
  // syntax.
  FixIts.erase(std::unique(FixIts.begin(), FixIts.end(),
                           [](const llvm::SMFixIt &lhs,
                              const llvm::SMFixIt &rhs) -> bool {
                             return lhs.getRange().Start ==
                                        rhs.getRange().Start &&
                                    lhs.getRange().End == rhs.getRange().End &&
                                    lhs.getText() == rhs.getText();
                           }),
               FixIts.end());
  // Filter out overlapping fix-its. This allows the compiler to apply changes
  // to the easy parts of the file, and leave in the tricky cases for the
  // developer to handle manually.
  FixIts.erase(swift::removeAdjacentIf(
                   FixIts.begin(), FixIts.end(),
                   [](const llvm::SMFixIt &lhs, const llvm::SMFixIt &rhs) {
                     return lhs.getRange().End.getPointer() >
                            rhs.getRange().Start.getPointer();
                   }),
               FixIts.end());

  // Get the contents of the original source file.
  auto memBuffer = SM.getLLVMSourceMgr().getMemoryBuffer(BufferID);
  auto bufferRange = memBuffer->getBuffer();

  // Apply the fixes, building up a new buffer as an std::string.
  const char *LastPos = bufferRange.begin();
  std::string Result;

  for (auto &fix : FixIts) {
    // We cannot handle overlapping fixits, so assert that they don't happen.
    assert(LastPos <= fix.getRange().Start.getPointer() &&
           "Cannot handle overlapping fixits");

    // Keep anything from the last spot we've checked to the start of the fixit.
    Result.append(LastPos, fix.getRange().Start.getPointer());

    // Replace the content covered by the fixit with the replacement text.
    Result.append(fix.getText().begin(), fix.getText().end());

    // Next character to consider is at the end of the fixit.
    LastPos = fix.getRange().End.getPointer();
  }

  // Retain the end of the file.
  Result.append(LastPos, bufferRange.end());

  std::error_code error;
  llvm::raw_fd_ostream outs(memBuffer->getBufferIdentifier(), error,
                            llvm::sys::fs::OpenFlags::F_None);
  if (!error)
    outs << Result;
}

/// diagnostics for '<unknown>:0' should be considered as unexpected.
static bool
verifyUnknown(SourceManager &SM,
              std::vector<CapturedDiagnosticInfo> &CapturedDiagnostics) {
  bool HadError = false;
  for (unsigned i = 0, e = CapturedDiagnostics.size(); i != e; ++i) {
    if (CapturedDiagnostics[i].Loc.isValid())
      continue;

    HadError = true;
    std::string Message =
        ("unexpected " +
         getDiagKindString(CapturedDiagnostics[i].Classification) +
         " produced: " + CapturedDiagnostics[i].Message)
            .str();

    auto diag = SM.GetMessage({}, llvm::SourceMgr::DK_Error, Message, {}, {});
    SM.getLLVMSourceMgr().PrintMessage(llvm::errs(), diag);
  }
  return HadError;
}

static unsigned getColumnNumber(StringRef buffer, llvm::SMLoc loc) {
  assert(loc.getPointer() >= buffer.data());
  assert((size_t)(loc.getPointer() - buffer.data()) <= buffer.size());

  StringRef UpToLoc = buffer.slice(0, loc.getPointer() - buffer.data());

  size_t ColumnNo = UpToLoc.size();
  size_t NewlinePos = UpToLoc.find_last_of("\r\n");
  if (NewlinePos != StringRef::npos)
    ColumnNo -= NewlinePos;

  return static_cast<unsigned>(ColumnNo);
}

/// Return true if the given \p ExpectedFixIt is in the fix-its emitted by
/// diagnostic \p D.
bool DiagnosticVerifier::checkForFixIt(const ExpectedFixIt &Expected,
                                       const CapturedDiagnosticInfo &D,
                                       StringRef buffer) {
  for (auto &ActualFixIt : D.FixIts) {
    if (ActualFixIt.getText() != Expected.Text)
      continue;

    CharSourceRange Range = ActualFixIt.getRange();
    if (getColumnNumber(buffer, getRawLoc(Range.getStart())) !=
        Expected.StartCol)
      continue;
    if (getColumnNumber(buffer, getRawLoc(Range.getEnd())) != Expected.EndCol)
      continue;

    return true;
  }

  return false;
}

std::string
DiagnosticVerifier::renderFixits(ArrayRef<DiagnosticInfo::FixIt> fixits,
                                 StringRef InputFile) {
  std::string Result;
  llvm::raw_string_ostream OS(Result);
  interleave(fixits,
             [&](const DiagnosticInfo::FixIt &ActualFixIt) {
               CharSourceRange Range = ActualFixIt.getRange();

               OS << "{{"
                  << getColumnNumber(InputFile, getRawLoc(Range.getStart()))
                  << '-'
                  << getColumnNumber(InputFile, getRawLoc(Range.getEnd()))
                  << '=';

               for (auto C : ActualFixIt.getText()) {
                 if (C == '\n')
                   OS << "\\n";
                 else if (C == '}' || C == '\\')
                   OS << '\\' << C;
                 else
                   OS << C;
               }
               OS << "}}";
             },
             [&] { OS << ' '; });
  return OS.str();
}

static void parseExpectedDiagnosticsFromBuffer(
    unsigned int BufferID, SourceLoc BufferStartLoc,
    std::vector<ExpectedDiagnosticInfo> &ExpectedDiagnostics,
    StringRef InputFile, SourceManager &SM,
    std::vector<llvm::SMDiagnostic> &Errors) {
  unsigned PrevExpectedContinuationLine = 0;

  auto addError = [&](const char *Loc, const Twine &message,
                      ArrayRef<llvm::SMFixIt> FixIts = {}) {
    auto loc = SourceLoc(llvm::SMLoc::getFromPointer(Loc));
    auto diag =
        SM.GetMessage(loc, llvm::SourceMgr::DK_Error, message, {}, FixIts);
    if (AbortOnDiagnosticNotBeingProduced)
      llvm_unreachable("Aborting due to failure to parse a diagnostic.");
    Errors.push_back(diag);
  };

  for (size_t Match = InputFile.find("expected-");
       Match != StringRef::npos; Match = InputFile.find("expected-", Match+1)) {
    // Process this potential match.  If we fail to process it, just move on to
    // the next match.
    StringRef MatchStart = InputFile.substr(Match);
    const char *DiagnosticLoc = MatchStart.data();

    DiagnosticKind ExpectedClassification;
    if (MatchStart.startswith("expected-note")) {
      ExpectedClassification = DiagnosticKind::Note;
      MatchStart = MatchStart.substr(strlen("expected-note"));
    } else if (MatchStart.startswith("expected-warning")) {
      ExpectedClassification = DiagnosticKind::Warning;
      MatchStart = MatchStart.substr(strlen("expected-warning"));
    } else if (MatchStart.startswith("expected-error")) {
      ExpectedClassification = DiagnosticKind::Error;
      MatchStart = MatchStart.substr(strlen("expected-error"));
    } else if (MatchStart.startswith("expected-remark")) {
      ExpectedClassification = DiagnosticKind::Remark;
      MatchStart = MatchStart.substr(strlen("expected-remark"));
    } else
      continue;

    // Skip any whitespace before the {{.
    MatchStart = MatchStart.substr(MatchStart.find_first_not_of(" \t"));

    size_t TextStartIdx = MatchStart.find("{{");
    if (TextStartIdx == StringRef::npos) {
      addError(MatchStart.data(),
               "expected {{ in expected-warning/note/error line", {});
      continue;
    }

    ExpectedDiagnosticInfo Expected(DiagnosticLoc, ExpectedClassification);
    int LineOffset = 0;

    if (TextStartIdx > 0 && MatchStart[0] == '@') {
      if (MatchStart[1] != '+' && MatchStart[1] != '-' &&
          MatchStart[1] != ':') {
        addError(MatchStart.data(),
                 "expected '+'/'-' for line offset, or ':' for column", {});
        continue;
      }
      StringRef Offs;
      if (MatchStart[1] == '+')
        Offs = MatchStart.slice(2, TextStartIdx).rtrim();
      else
        Offs = MatchStart.slice(1, TextStartIdx).rtrim();

      size_t SpaceIndex = Offs.find(' ');
      if (SpaceIndex != StringRef::npos && SpaceIndex < TextStartIdx) {
        size_t Delta = Offs.size() - SpaceIndex;
        MatchStart = MatchStart.substr(TextStartIdx - Delta);
        TextStartIdx = Delta;
        Offs = Offs.slice(0, SpaceIndex);
      } else {
        MatchStart = MatchStart.substr(TextStartIdx);
        TextStartIdx = 0;
      }

      size_t ColonIndex = Offs.find(':');
      // Check whether a line offset was provided
      if (ColonIndex != 0) {
        StringRef LineOffs = Offs.slice(0, ColonIndex);
        if (LineOffs.getAsInteger(10, LineOffset)) {
          addError(MatchStart.data(), "expected line offset before '{{'", {});
          continue;
        }
      }

      // Check whether a column was provided
      if (ColonIndex != StringRef::npos) {
        Offs = Offs.slice(ColonIndex + 1, Offs.size());
        int Column = 0;
        if (Offs.getAsInteger(10, Column)) {
          addError(MatchStart.data(), "expected column before '{{'", {});
          continue;
        }
        Expected.ColumnNo = Column;
      }
    }

    unsigned Count = 1;
    if (TextStartIdx > 0) {
      StringRef CountStr = MatchStart.substr(0, TextStartIdx).trim();
      if (CountStr == "*") {
        Expected.mayAppear = true;
      } else {
        if (CountStr.getAsInteger(10, Count)) {
          addError(MatchStart.data(), "expected match count before '{{'", {});
          continue;
        }
        if (Count == 0) {
          addError(MatchStart.data(),
                   "expected positive match count before '{{'", {});
          continue;
        }
      }

      // Resync up to the '{{'.
      MatchStart = MatchStart.substr(TextStartIdx);
    }

    size_t End = MatchStart.find("}}");
    if (End == StringRef::npos) {
      addError(
          MatchStart.data(),
          "didn't find '}}' to match '{{' in expected-warning/note/error line",
          {});
      continue;
    }

    llvm::SmallString<256> Buf;
    Expected.MessageRange = MatchStart.slice(2, End);
    Expected.MessageStr =
        Lexer::getEncodedStringSegment(Expected.MessageRange, Buf).str();
    if (PrevExpectedContinuationLine)
      Expected.LineNo = PrevExpectedContinuationLine;
    else
      Expected.LineNo = SM.getPresumedLineAndColumnForLoc(
                              BufferStartLoc.getAdvancedLoc(MatchStart.data() -
                                                            InputFile.data()),
                              BufferID)
                            .first;
    Expected.LineNo += LineOffset;

    // Check if the next expected diagnostic should be in the same line.
    StringRef AfterEnd = MatchStart.substr(End + strlen("}}"));
    AfterEnd = AfterEnd.substr(AfterEnd.find_first_not_of(" \t"));
    if (AfterEnd.startswith("\\"))
      PrevExpectedContinuationLine = Expected.LineNo;
    else
      PrevExpectedContinuationLine = 0;

    // Scan for fix-its: {{10-14=replacement text}}
    StringRef ExtraChecks = MatchStart.substr(End+2).ltrim(" \t");
    while (ExtraChecks.startswith("{{")) {
      // First make sure we have a closing "}}".
      size_t EndIndex = ExtraChecks.find("}}");
      if (EndIndex == StringRef::npos) {
        addError(ExtraChecks.data(),
                 "didn't find '}}' to match '{{' in diagnostic verification",
                 {});
        break;
      }

      // Allow for close braces to appear in the replacement text.
      while (EndIndex + 2 < ExtraChecks.size() &&
             ExtraChecks[EndIndex + 2] == '}')
        ++EndIndex;

      const char *OpenLoc = ExtraChecks.data(); // Beginning of opening '{{'.
      const char *CloseLoc =
          ExtraChecks.data() + EndIndex + 2; // End of closing '}}'.

      StringRef CheckStr = ExtraChecks.slice(2, EndIndex);
      // Check for matching a later "}}" on a different line.
      if (CheckStr.find_first_of("\r\n") != StringRef::npos) {
        addError(ExtraChecks.data(),
                 "didn't find '}}' to match '{{' in "
                 "diagnostic verification",
                 {});
        break;
      }

      // Prepare for the next round of checks.
      ExtraChecks = ExtraChecks.substr(EndIndex + 2).ltrim();

      // If this check starts with 'educational-notes=', check for one or more
      // educational notes instead of a fix-it.
      if (CheckStr.startswith(educationalNotesSpecifier)) {
        if (Expected.EducationalNotes.hasValue()) {
          addError(CheckStr.data(),
                   "each verified diagnostic may only have one "
                   "{{educational-notes=<#notes#>}} declaration",
                   {});
          continue;
        }
        StringRef NotesStr = CheckStr.substr(
            educationalNotesSpecifier.size()); // Trim 'educational-notes='.
        llvm::SmallVector<StringRef, 1> names;
        // Note names are comma-separated.
        std::pair<StringRef, StringRef> split;
        do {
          split = NotesStr.split(',');
          names.push_back(split.first);
          NotesStr = split.second;
        } while (!NotesStr.empty());
        Expected.EducationalNotes.emplace(OpenLoc, CloseLoc, names);
        continue;
      }

      // This wasn't an educational notes specifier, so it must be a fix-it.
      // Special case for specifying no fixits should appear.
      if (CheckStr == fixitExpectationNoneString) {
        if (Expected.noneMarkerStartLoc) {
          addError(
              CheckStr.data() - 2,
              Twine("A second {{") + fixitExpectationNoneString +
                  "}} was found. It may only appear once in an expectation.",
              {});
          break;
        }

        Expected.noneMarkerStartLoc = CheckStr.data() - 2;
        continue;
      }

      if (Expected.noneMarkerStartLoc) {
        addError(Expected.noneMarkerStartLoc,
                 Twine("{{") + fixitExpectationNoneString +
                     "}} must be at the end.",
                 {});
        break;
      }

      // Parse the pieces of the fix-it.
      size_t MinusLoc = CheckStr.find('-');
      if (MinusLoc == StringRef::npos) {
        addError(CheckStr.data(), "expected '-' in fix-it verification", {});
        continue;
      }
      StringRef StartColStr = CheckStr.slice(0, MinusLoc);
      StringRef AfterMinus = CheckStr.substr(MinusLoc + 1);

      size_t EqualLoc = AfterMinus.find('=');
      if (EqualLoc == StringRef::npos) {
        addError(AfterMinus.data(),
                 "expected '=' after '-' in fix-it verification", {});
        continue;
      }
      StringRef EndColStr = AfterMinus.slice(0, EqualLoc);
      StringRef AfterEqual = AfterMinus.substr(EqualLoc+1);

      ExpectedFixIt FixIt;
      FixIt.StartLoc = OpenLoc;
      FixIt.EndLoc = CloseLoc;
      if (StartColStr.getAsInteger(10, FixIt.StartCol)) {
        addError(StartColStr.data(),
                 "invalid column number in fix-it verification", {});
        continue;
      }
      if (EndColStr.getAsInteger(10, FixIt.EndCol)) {
        addError(EndColStr.data(),
                 "invalid column number in fix-it verification", {});
        continue;
      }

      // Translate literal "\\n" into '\n', inefficiently.
      StringRef fixItText = AfterEqual.slice(0, EndIndex);
      for (const char *current = fixItText.begin(), *end = fixItText.end();
           current != end; /* in loop */) {
        if (*current == '\\' && current + 1 < end) {
          if (current[1] == 'n') {
            FixIt.Text += '\n';
            current += 2;
          } else {  // Handle \}, \\, etc.
            FixIt.Text += current[1];
            current += 2;
          }

        } else {
          FixIt.Text += *current++;
        }
      }

      Expected.Fixits.push_back(FixIt);
    }

    Expected.ExpectedEnd = ExtraChecks.data();

    // Don't include trailing whitespace in the expected-foo{{}} range.
    while (isspace(Expected.ExpectedEnd[-1]))
      --Expected.ExpectedEnd;

    // Add the diagnostic the expected number of times.
    for (; Count; --Count)
      ExpectedDiagnostics.push_back(Expected);
  }
}

void DiagnosticVerifier::verifyAllExpectedDiagnosticsAppeared(
    StringRef BufferName, StringRef InputFile,
    std::vector<ExpectedDiagnosticInfo> &ExpectedDiagnostics,
    std::vector<llvm::SMDiagnostic> &Errors) {
  using llvm::SMLoc;

  auto addError = [&](const char *Loc, const Twine &message,
                      ArrayRef<llvm::SMFixIt> FixIts = {}) {
    auto loc = SourceLoc(SMLoc::getFromPointer(Loc));
    auto diag =
        SM.GetMessage(loc, llvm::SourceMgr::DK_Error, message, {}, FixIts);
    if (AbortOnMalformedDiagnostic)
      llvm_unreachable("Found malformed diagnostic?!");
    Errors.push_back(diag);
  };

  for (unsigned i = ExpectedDiagnostics.size(); i != 0; ) {
    --i;
    auto &expected = ExpectedDiagnostics[i];

    // Check to see if we had this expected diagnostic.
    auto FoundDiagnosticIter =
        findDiagnostic(CapturedDiagnostics, expected, BufferName);
    if (FoundDiagnosticIter == CapturedDiagnostics.end()) {
      // Diagnostic didn't exist.  If this is a 'mayAppear' diagnostic, then
      // we're ok.  Otherwise, leave it in the list.
      if (expected.mayAppear)
        ExpectedDiagnostics.erase(ExpectedDiagnostics.begin()+i);
      continue;
    }

    auto &FoundDiagnostic = *FoundDiagnosticIter;

    const char *missedFixitLoc = nullptr;
    // Verify that any expected fix-its are present in the diagnostic.
    for (auto fixit : expected.Fixits) {
      // If we found it, we're ok.
      if (!checkForFixIt(fixit, FoundDiagnostic, InputFile)) {
        missedFixitLoc = fixit.StartLoc;
        break;
      }
    }

    const bool isUnexpectedFixitsSeen =
        expected.Fixits.size() < FoundDiagnostic.FixIts.size();

    struct ActualFixitsPhrase {
      std::string phrase;
      std::string actualFixits;
    };

    auto makeActualFixitsPhrase =
        [&](ArrayRef<DiagnosticInfo::FixIt> actualFixits)
        -> ActualFixitsPhrase {
      std::string actualFixitsStr = renderFixits(actualFixits, InputFile);

      return ActualFixitsPhrase{(Twine("actual fix-it") +
                                 (actualFixits.size() >= 2 ? "s" : "") +
                                 " seen: " + actualFixitsStr)
                                    .str(),
                                actualFixitsStr};
    };

    auto emitFixItsError = [&](const char *location, const Twine &message,
                               const char *replStartLoc, const char *replEndLoc,
                               const std::string &replStr) {
      llvm::SMFixIt fix(llvm::SMRange(SMLoc::getFromPointer(replStartLoc),
                                      SMLoc::getFromPointer(replEndLoc)),
                        replStr);
      if (AbortOnUnexpectedDiagnosticFixit)
        llvm_unreachable("Found unexpected fixit!");
      addError(location, message, fix);
    };

    // If we have any expected fixits that didn't get matched, then they are
    // wrong.  Replace the failed fixit with what actually happened.

    if (missedFixitLoc) {
      // If we had an incorrect expected fixit, render it and produce a fixit
      // of our own.

      assert(!expected.Fixits.empty() &&
             "some fix-its should be expected here");

      const char *replStartLoc = expected.Fixits.front().StartLoc;
      const char *replEndLoc = expected.Fixits.back().EndLoc;

      std::string message = "expected fix-it not seen";
      std::string actualFixits;

      if (FoundDiagnostic.FixIts.empty()) {
        /// If actual fix-its is empty,
        /// eat a space before first marker.
        /// For example,
        ///
        /// @code
        /// expected-error {{message}} {{1-2=aa}}
        ///                           ~~~~~~~~~~~
        ///                           ^ remove
        /// @endcode
        if (replStartLoc[-1] == ' ') {
          --replStartLoc;
        }
      } else {
        auto phrase = makeActualFixitsPhrase(FoundDiagnostic.FixIts);
        actualFixits = phrase.actualFixits;
        message += "; " + phrase.phrase;
      }

      emitFixItsError(missedFixitLoc, message, replStartLoc, replEndLoc,
                      actualFixits);
    } else if (expected.noExtraFixitsMayAppear() && isUnexpectedFixitsSeen) {
      // If unexpected fixit were produced, add a fixit to add them in.

      assert(!FoundDiagnostic.FixIts.empty() &&
             "some fix-its should be produced here");
      assert(expected.noneMarkerStartLoc && "none marker location is null");

      const char *replStartLoc = nullptr, *replEndLoc = nullptr;
      std::string message;
      if (expected.Fixits.empty()) {
        message = "expected no fix-its";
        replStartLoc = expected.noneMarkerStartLoc;
        replEndLoc = expected.noneMarkerStartLoc;
      } else {
        message = "unexpected fix-it seen";
        replStartLoc = expected.Fixits.front().StartLoc;
        replEndLoc = expected.Fixits.back().EndLoc;
      }

      auto phrase = makeActualFixitsPhrase(FoundDiagnostic.FixIts);
      std::string actualFixits = phrase.actualFixits;
      message += "; " + phrase.phrase;

      if (replStartLoc == replEndLoc) {
        /// If no fix-its was expected and range of replacement is empty,
        /// insert space after new last marker.
        /// For example:
        ///
        /// @code
        /// expected-error {{message}} {{none}}
        ///                            ^
        ///                    insert `{{1-2=aa}} `
        /// @endcode
        actualFixits += " ";
      }

      emitFixItsError(expected.noneMarkerStartLoc, message, replStartLoc,
                      replEndLoc, actualFixits);
    }

    if (auto expectedNotes = expected.EducationalNotes) {
      // Verify educational notes
      for (auto &foundName : FoundDiagnostic.EducationalNotes) {
        llvm::erase_if(expectedNotes->Names,
                       [&](StringRef item) { return item.equals(foundName); });
      }

      if (!expectedNotes->Names.empty()) {
        if (FoundDiagnostic.EducationalNotes.empty()) {
          addError(expectedNotes->StartLoc,
                   "expected educational note(s) not seen");
        } else {
          // If we had an incorrect expected note, render it and produce a fixit
          // of our own.
          auto actual =
              renderEducationalNotes(FoundDiagnostic.EducationalNotes);
          auto replStartLoc = SMLoc::getFromPointer(expectedNotes->StartLoc);
          auto replEndLoc = SMLoc::getFromPointer(expectedNotes->EndLoc);

          llvm::SMFixIt fix(llvm::SMRange(replStartLoc, replEndLoc), actual);
          addError(expectedNotes->StartLoc,
                   "expected educational note(s) not seen; actual educational "
                   "note(s): " + actual, fix);
        }
      }
    }

    // Actually remove the diagnostic from the list, so we don't match it
    // again. We do have to do this after checking fix-its, though, because
    // the diagnostic owns its fix-its.
    CapturedDiagnostics.erase(FoundDiagnosticIter);

    // We found the diagnostic, so remove it... unless we allow an arbitrary
    // number of diagnostics, in which case we want to reprocess this.
    if (expected.mayAppear)
      ++i;
    else
      ExpectedDiagnostics.erase(ExpectedDiagnostics.begin()+i);
  }
}

void DiagnosticVerifier::diagnoseIncorrectDiagnostics(
    StringRef BufferName, std::vector<llvm::SMDiagnostic> &Errors,
    std::vector<ExpectedDiagnosticInfo> &ExpectedDiagnostics) {
  using llvm::SMLoc;
  auto addError = [&](const char *Loc, const Twine &message,
                      ArrayRef<llvm::SMFixIt> FixIts = {}) {
    auto loc = SourceLoc(SMLoc::getFromPointer(Loc));
    auto diag =
        SM.GetMessage(loc, llvm::SourceMgr::DK_Error, message, {}, FixIts);
    if (AbortOnIncorrectDiagnostic)
      llvm_unreachable("Found incorrect diagnostic?!");
    Errors.push_back(diag);
  };

  auto expectedDiagIter = ExpectedDiagnostics.begin();
  while (expectedDiagIter != ExpectedDiagnostics.end()) {
    // Check to see if any found diagnostics have the right line and
    // classification, but the wrong text.
    auto I = CapturedDiagnostics.begin();
    for (auto E = CapturedDiagnostics.end(); I != E; ++I) {
      // Verify the file and line of the diagnostic.
      if (I->Line != expectedDiagIter->LineNo || I->FileName != BufferName ||
          I->Classification != expectedDiagIter->Classification)
        continue;

      // Otherwise, we found it, break out.
      break;
    }

    if (I == CapturedDiagnostics.end()) {
      ++expectedDiagIter;
      continue;
    }

    if (I->Message.find(expectedDiagIter->MessageStr) == StringRef::npos) {
      auto StartLoc =
          SMLoc::getFromPointer(expectedDiagIter->MessageRange.begin());
      auto EndLoc = SMLoc::getFromPointer(expectedDiagIter->MessageRange.end());

      llvm::SMFixIt fixIt(llvm::SMRange{StartLoc, EndLoc}, I->Message);
      addError(expectedDiagIter->MessageRange.begin(),
               "incorrect message found", fixIt);
    } else if (I->Column != *expectedDiagIter->ColumnNo) {
      // The difference must be only in the column
      addError(expectedDiagIter->MessageRange.begin(),
               llvm::formatv("message found at column {0} but was expected to "
                             "appear at column {1}",
                             I->Column, *expectedDiagIter->ColumnNo));
    } else {
      llvm_unreachable("unhandled difference from expected diagnostic");
    }
    CapturedDiagnostics.erase(I);
    expectedDiagIter = ExpectedDiagnostics.erase(expectedDiagIter);
  }
}

void DiagnosticVerifier::diagnoseExpectedDiagnosticsThatDidntAppear(
    StringRef InputFile, ArrayRef<ExpectedDiagnosticInfo> ExpectedDiagnostics,
    std::vector<llvm::SMDiagnostic> &Errors) {
  using llvm::SMLoc;

  auto addError = [&](const char *Loc, const Twine &message,
                      ArrayRef<llvm::SMFixIt> FixIts = {}) {
    auto loc = SourceLoc(SMLoc::getFromPointer(Loc));
    auto diag =
        SM.GetMessage(loc, llvm::SourceMgr::DK_Error, message, {}, FixIts);
    if (AbortOnMissingDiagnostic)
      llvm_unreachable("Found missing diagnostic?!");
    Errors.push_back(diag);
  };

  for (auto const &expected : ExpectedDiagnostics) {
    std::string message = "expected " +
                          getDiagKindString(expected.Classification) +
                          " not produced";

    // Get the range of the expected-foo{{}} diagnostic specifier.
    auto StartLoc = expected.ExpectedStart;
    auto EndLoc = expected.ExpectedEnd;

    // A very common case if for the specifier to be the last thing on the line.
    // In this case, eat any trailing whitespace.
    while (isspace(*EndLoc) && *EndLoc != '\n' && *EndLoc != '\r')
      ++EndLoc;

    // If we found the end of the line, we can do great things.  Otherwise,
    // avoid nuking whitespace that might be zapped through other means.
    if (*EndLoc != '\n' && *EndLoc != '\r') {
      EndLoc = expected.ExpectedEnd;
    } else {
      // If we hit the end of line, then zap whitespace leading up to it.
      auto FileStart = InputFile.data();
      while (StartLoc-1 != FileStart && isspace(StartLoc[-1]) &&
             StartLoc[-1] != '\n' && StartLoc[-1] != '\r')
        --StartLoc;

      // If we got to the end of the line, and the thing before this diagnostic
      // is a "//" then we can remove it too.
      if (StartLoc-2 >= FileStart && StartLoc[-1] == '/' && StartLoc[-2] == '/')
        StartLoc -= 2;

      // Perform another round of general whitespace nuking to cleanup
      // whitespace before the //.
      while (StartLoc-1 != FileStart && isspace(StartLoc[-1]) &&
             StartLoc[-1] != '\n' && StartLoc[-1] != '\r')
        --StartLoc;

      // If we found a \n, then we can nuke the entire line.
      if (StartLoc-1 != FileStart &&
          (StartLoc[-1] == '\n' || StartLoc[-1] == '\r'))
        --StartLoc;
    }

    // Remove the expected-foo{{}} as a fixit.
    llvm::SMFixIt fixIt(llvm::SMRange{
      SMLoc::getFromPointer(StartLoc),
      SMLoc::getFromPointer(EndLoc)
    }, "");
    addError(expected.ExpectedStart, message, fixIt);
  }
}

/// After the file has been processed, check to see if we got all of
/// the expected diagnostics and check to see if there were any unexpected
/// ones.
DiagnosticVerifier::Result DiagnosticVerifier::verifyFile(unsigned BufferID) {
  using llvm::SMLoc;

  const SourceLoc BufferStartLoc = SM.getLocForBufferStart(BufferID);
  CharSourceRange EntireRange = SM.getRangeForBuffer(BufferID);
  StringRef InputFile = SM.extractText(EntireRange);
  StringRef BufferName = SM.getIdentifierForBuffer(BufferID);

  // Queue up all of the diagnostics, allowing us to sort them and emit them in
  // file order.
  std::vector<llvm::SMDiagnostic> Errors;
  std::vector<ExpectedDiagnosticInfo> ExpectedDiagnostics;

  // Scan the memory buffer looking for expected-note/warning/error.
  parseExpectedDiagnosticsFromBuffer(
      BufferID, BufferStartLoc, ExpectedDiagnostics, InputFile, SM, Errors);

  // Then verify that all the expected diagnostics actually appeared.
  std::reverse(ExpectedDiagnostics.begin(), ExpectedDiagnostics.end());
  verifyAllExpectedDiagnosticsAppeared(BufferName, InputFile,
                                       ExpectedDiagnostics, Errors);

  // Check to see if we have any incorrect diagnostics.  If so, diagnose them as
  // such.
  diagnoseIncorrectDiagnostics(BufferName, Errors, ExpectedDiagnostics);

  // Diagnose expected diagnostics that didn't appear.
  std::reverse(ExpectedDiagnostics.begin(), ExpectedDiagnostics.end());
  diagnoseExpectedDiagnosticsThatDidntAppear(InputFile, ExpectedDiagnostics,
                                             Errors);

  // Verify that there are no diagnostics (in MemoryBuffer) left in the list.
  auto addError = [&](const char *Loc, const Twine &message,
                      ArrayRef<llvm::SMFixIt> FixIts = {}) {
    auto loc = SourceLoc(llvm::SMLoc::getFromPointer(Loc));
    auto diag =
        SM.GetMessage(loc, llvm::SourceMgr::DK_Error, message, {}, FixIts);
    if (AbortOnUnexpectedDiagnostic)
      llvm_unreachable("Found unexpected diagnostic?!");
    Errors.push_back(diag);
  };
  bool HadUnexpectedDiag = false;
  auto CapturedDiagIter = CapturedDiagnostics.begin();
  while (CapturedDiagIter != CapturedDiagnostics.end()) {
    if (CapturedDiagIter->FileName != BufferName) {
      ++CapturedDiagIter;
      continue;
    }

    HadUnexpectedDiag = true;
    std::string Message =
        ("unexpected " + getDiagKindString(CapturedDiagIter->Classification) +
         " produced: " + CapturedDiagIter->Message)
            .str();
    addError(getRawLoc(CapturedDiagIter->Loc).getPointer(), Message);
    CapturedDiagIter = CapturedDiagnostics.erase(CapturedDiagIter);
  }

  // Ok. Now we have finished processiing and found all of the errors that we
  // are going to emit.
  //
  // With that in mind, sort the diagnostics by their address in the memory
  // buffer as the primary key.  This ensures that an "unexpected diagnostic"
  // and "expected diagnostic" in the same place are emitted next to each other.
  std::sort(Errors.begin(), Errors.end(),
            [&](const llvm::SMDiagnostic &lhs,
                const llvm::SMDiagnostic &rhs) -> bool {
              return lhs.getLoc().getPointer() < rhs.getLoc().getPointer();
            });

  // Then emit all of the queue'd up errors.
  for (auto Err : Errors)
    SM.getLLVMSourceMgr().PrintMessage(llvm::errs(), Err);

  // If auto-apply fixits is on, rewrite the original source file.
  if (AutoApplyFixes)
    autoApplyFixes(SM, BufferID, Errors);

  return Result{!Errors.empty(), HadUnexpectedDiag};
}

void DiagnosticVerifier::printRemainingDiagnostics() const {
  for (const auto &diag : CapturedDiagnostics) {
    // Determine what kind of diagnostic we're emitting.
    llvm::SourceMgr::DiagKind SMKind;
    switch (diag.Classification) {
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

    SM.getLLVMSourceMgr().PrintMessage(
        llvm::errs(), getRawLoc(diag.Loc), SMKind,
        "diagnostic produced elsewhere: " + diag.Message.str(),
        /*Ranges=*/{}, {});
  }
}

//===----------------------------------------------------------------------===//
// Main entrypoints
//===----------------------------------------------------------------------===//

/// Every time a diagnostic is generated in -verify mode, this function is
/// called with the diagnostic.  We just buffer them up until the end of the
/// file.
void DiagnosticVerifier::handleDiagnostic(SourceManager &SM,
                                          const DiagnosticInfo &Info) {
  SmallVector<DiagnosticInfo::FixIt, 2> fixIts;
  std::copy(Info.FixIts.begin(), Info.FixIts.end(), std::back_inserter(fixIts));

  llvm::SmallVector<std::string, 1> eduNotes;
  for (auto &notePath : Info.EducationalNotePaths) {
    eduNotes.push_back(llvm::sys::path::stem(notePath).str());
  }

  llvm::SmallString<128> message;
  {
    llvm::raw_svector_ostream Out(message);
    DiagnosticEngine::formatDiagnosticText(Out, Info.FormatString,
                                           Info.FormatArgs);
  }

  if (Info.Loc.isValid()) {
    const auto lineAndColumn = SM.getPresumedLineAndColumnForLoc(Info.Loc);
    const auto fileName = SM.getDisplayNameForLoc(Info.Loc);
    CapturedDiagnostics.emplace_back(message, fileName, Info.Kind, Info.Loc,
                                     lineAndColumn.first, lineAndColumn.second,
                                     fixIts, eduNotes);
  } else {
    CapturedDiagnostics.emplace_back(message, StringRef(), Info.Kind, Info.Loc,
                                     0, 0, fixIts, eduNotes);
  }

  // If this diagnostic came from a different SourceManager (as can happen
  // while compiling a module interface), translate its SourceLocs to match the
  // verifier's SourceManager.
  if (&SM != &(this->SM)) {
    auto &capturedDiag = CapturedDiagnostics.back();
    auto &correctSM = this->SM;

    capturedDiag.Loc = correctSM.getLocForForeignLoc(capturedDiag.Loc, SM);
    for (auto &fixIt : capturedDiag.FixIts) {
      auto newStart = correctSM.getLocForForeignLoc(fixIt.getRange().getStart(),
                                                    SM);
      fixIt.getRange() = CharSourceRange(newStart,
                                         fixIt.getRange().getByteLength());
    }
  }
}

/// Once all diagnostics have been captured, perform verification.
bool DiagnosticVerifier::finishProcessing() {
  DiagnosticVerifier::Result Result = {false, false};

  ArrayRef<unsigned> BufferIDLists[2] = { BufferIDs, AdditionalBufferIDs };
  for (ArrayRef<unsigned> BufferIDList : BufferIDLists)
    for (auto &BufferID : BufferIDList) {
      DiagnosticVerifier::Result FileResult = verifyFile(BufferID);
      Result.HadError |= FileResult.HadError;
      Result.HadUnexpectedDiag |= FileResult.HadUnexpectedDiag;
    }
  if (!IgnoreUnknown) {
    bool HadError = verifyUnknown(SM, CapturedDiagnostics);
    Result.HadError |= HadError;
    // For <unknown>, all errors are unexpected.
    Result.HadUnexpectedDiag |= HadError;
  }

  if (Result.HadUnexpectedDiag)
    printRemainingDiagnostics();

  return Result.HadError;
}
