//===--- DiagnosticBridge.h - Diagnostic Bridge to SwiftSyntax --*- C++ -*-===//
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
//
//  This file declares the DiagnosticBridge class, which bridges to swift-syntax
//  for diagnostics printing.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_BASIC_DIAGNOSTICBRIDGE_H
#define SWIFT_BASIC_DIAGNOSTICBRIDGE_H

#include "swift/AST/DiagnosticConsumer.h"
#include "swift/Basic/SourceManager.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/raw_ostream.h"

namespace swift {
class DiagnosticBridge {
  /// A queued up source file known to the queued diagnostics.
  using QueuedBuffer = void *;

  /// The queued diagnostics structure.
  void *queuedDiagnostics = nullptr;
  llvm::DenseMap<unsigned, QueuedBuffer> queuedBuffers;

  /// Source file syntax nodes cached by { source manager, buffer ID }.
  llvm::DenseMap<std::pair<SourceManager *, unsigned>, void *> sourceFileSyntax;

public:
  /// Enqueue diagnostics.
  void enqueueDiagnostic(SourceManager &SM, const DiagnosticInfo &Info,
                         unsigned innermostBufferID);

  /// Flush all enqueued diagnostics.
  void flush(llvm::raw_ostream &OS, bool includeTrailingBreak,
             bool forceColors);

  /// Retrieve the stack of source buffers from the provided location out to
  /// a physical source file, with source buffer IDs for each step along the way
  /// due to (e.g.) macro expansions or generated code.
  ///
  /// The resulting vector will always contain valid source locations. If the
  /// initial location is invalid, the result will be empty.
  static SmallVector<unsigned, 1> getSourceBufferStack(SourceManager &sourceMgr,
                                                       SourceLoc loc);

  DiagnosticBridge() = default;
  ~DiagnosticBridge();

private:
  /// Retrieve the SourceFileSyntax for the given buffer.
  void *getSourceFileSyntax(SourceManager &SM, unsigned bufferID,
                            StringRef displayName);

  void queueBuffer(SourceManager &sourceMgr, unsigned bufferID);
};
} // namespace swift

#endif
