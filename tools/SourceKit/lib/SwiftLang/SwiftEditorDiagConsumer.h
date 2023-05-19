//===--- SwiftEditorDiagConsumer.h - ----------------------------*- C++ -*-===//
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

#ifndef LLVM_SOURCEKIT_LIB_SWIFTLANG_SWIFTEDITORDIAGCONSUMER_H
#define LLVM_SOURCEKIT_LIB_SWIFTLANG_SWIFTEDITORDIAGCONSUMER_H

#include "SourceKit/Core/LangSupport.h"
#include "swift/AST/DiagnosticConsumer.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/MapVector.h"

namespace SourceKit {

class EditorDiagConsumer : public swift::DiagnosticConsumer {
  typedef std::vector<DiagnosticEntryInfo> DiagnosticsTy;
  /// Maps from a BufferID to the diagnostics that were emitted inside that
  /// buffer.
  llvm::DenseMap<unsigned, DiagnosticsTy> BufferDiagnostics;
  DiagnosticsTy InvalidLocDiagnostics;

  llvm::MapVector<StringRef, BufferInfoSharedPtr> BufferInfos;

  int LastDiagBufferID = -1;
  unsigned LastDiagIndex = 0;

  bool haveLastDiag() {
    return LastDiagBufferID >= 0;
  }
  void clearLastDiag() {
    LastDiagBufferID = -1;
  }
  DiagnosticEntryInfo &getLastDiag() {
    assert(haveLastDiag());
    return BufferDiagnostics[LastDiagBufferID][LastDiagIndex];
  }

  bool HadAnyError = false;

  BufferInfoSharedPtr getBufferInfo(StringRef FileName,
                                    Optional<unsigned> BufferID,
                                    swift::SourceManager &SM);

public:
  /// The diagnostics are returned in source-order.
  ArrayRef<DiagnosticEntryInfo> getDiagnosticsForBuffer(unsigned BufferID) const {
    ArrayRef<DiagnosticEntryInfo> Diags;
    auto DiagFound = BufferDiagnostics.find(BufferID);
    if (DiagFound != BufferDiagnostics.end())
      Diags = DiagFound->second;
    return Diags;
  }

  void getAllDiagnostics(SmallVectorImpl<DiagnosticEntryInfo> &Result);

  bool hadAnyError() const { return HadAnyError; }

  void handleDiagnostic(swift::SourceManager &SM,
                        const swift::DiagnosticInfo &Info) override;
};

} // namespace SourceKit

#endif
