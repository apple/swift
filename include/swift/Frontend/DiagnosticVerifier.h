//===--- DiagnosticVerifier.h - Diagnostic Verifier (-verify) ---*- C++ -*-===//
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
// This file exposes support for the diagnostic verifier, which is used to
// implement -verify mode in the compiler.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_FRONTEND_DIAGNOSTIC_VERIFIER_H
#define SWIFT_FRONTEND_DIAGNOSTIC_VERIFIER_H

#include "llvm/ADT/SmallString.h"
#include "swift/AST/DiagnosticConsumer.h"
#include "swift/Basic/LLVM.h"

namespace swift {
class DependencyTracker;
class FileUnit;
class SourceManager;
class SourceFile;

// MARK: - DependencyVerifier

bool verifyDependencies(SourceManager &SM, ArrayRef<FileUnit *> SFs);
bool verifyDependencies(SourceManager &SM, ArrayRef<SourceFile *> SFs);

// MARK: - DiagnosticVerifier
struct ExpectedFixIt;

struct CapturedDiagnosticInfo {
  llvm::SmallString<128> Message;
  llvm::SmallString<32> FileName;
  DiagnosticKind Classification;
  SourceLoc Loc;
  unsigned Line;
  unsigned Column;
  SmallVector<DiagnosticInfo::FixIt, 2> FixIts;
  SmallVector<std::string, 1> EducationalNotes;

  CapturedDiagnosticInfo(llvm::SmallString<128> Message,
                         llvm::SmallString<32> FileName,
                         DiagnosticKind Classification, SourceLoc Loc,
                         unsigned Line, unsigned Column,
                         SmallVector<DiagnosticInfo::FixIt, 2> FixIts,
                         SmallVector<std::string, 1> EducationalNotes)
      : Message(Message), FileName(FileName), Classification(Classification),
        Loc(Loc), Line(Line), Column(Column), FixIts(FixIts),
        EducationalNotes(EducationalNotes) {
    std::sort(EducationalNotes.begin(), EducationalNotes.end());
  }
};
/// This class implements support for -verify mode in the compiler.  It
/// buffers up diagnostics produced during compilation, then checks them
/// against expected-error markers in the source file.
class DiagnosticVerifier : public DiagnosticConsumer {
  SourceManager &SM;
  std::vector<CapturedDiagnosticInfo> CapturedDiagnostics;
  ArrayRef<unsigned> BufferIDs;
  SmallVector<unsigned, 4> AdditionalBufferIDs;
  bool AutoApplyFixes;
  bool IgnoreUnknown;

public:
  explicit DiagnosticVerifier(SourceManager &SM, ArrayRef<unsigned> BufferIDs,
                              bool AutoApplyFixes, bool IgnoreUnknown)
      : SM(SM), BufferIDs(BufferIDs), AutoApplyFixes(AutoApplyFixes),
        IgnoreUnknown(IgnoreUnknown) {}

  void appendAdditionalBufferID(unsigned bufferID) {
    AdditionalBufferIDs.push_back(bufferID);
  }

  virtual void handleDiagnostic(SourceManager &SM,
                                const DiagnosticInfo &Info) override;

  virtual bool finishProcessing() override;

  /// Forward declaration of struct only used in internal impl so it can be used
  /// in internal helper methods on this class. It is only declared, not defined
  /// in any header.
  struct ExpectedDiagnosticInfo;

private:
  /// Result of verifying a file.
  struct Result {
    /// Were there any errors? All of the following are considered errors:
    /// - Expected diagnostics that were not present
    /// - Unexpected diagnostics that were present
    /// - Errors in the definition of expected diagnostics
    bool HadError;
    bool HadUnexpectedDiag;
  };

  void verifyAllExpectedDiagnosticsAppeared(
      StringRef BufferName, StringRef InputFile,
      std::vector<ExpectedDiagnosticInfo> &ExpectedDiagnostics,
      std::vector<llvm::SMDiagnostic> &Errors);

  void diagnoseIncorrectDiagnostics(
      StringRef BufferName, std::vector<llvm::SMDiagnostic> &Errors,
      std::vector<ExpectedDiagnosticInfo> &ExpectedDiagnostics);

  void diagnoseExpectedDiagnosticsThatDidntAppear(
      StringRef InputFile, ArrayRef<ExpectedDiagnosticInfo> ExpectedDiagnostics,
      std::vector<llvm::SMDiagnostic> &Errors);

  /// verifyFile - After the file has been processed, check to see if we
  /// got all of the expected diagnostics and check to see if there were any
  /// unexpected ones.
  Result verifyFile(unsigned BufferID);

  bool checkForFixIt(const ExpectedFixIt &Expected,
                     const CapturedDiagnosticInfo &D, StringRef buffer);

  // Render the verifier syntax for a given set of fix-its.
  std::string renderFixits(ArrayRef<DiagnosticInfo::FixIt> fixits,
                           StringRef InputFile);

  void printRemainingDiagnostics() const;
};

} // end namespace swift

#endif
