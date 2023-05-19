//===--- CachedDiagnostics.h - Cached Diagnostics ---------------*- C++ -*-===//
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
//  This file defines the CachedDiagnosticConsumer class, which
//  caches the diagnostics which can be replayed with other DiagnosticConumers.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_CACHEDDIAGNOSTICS_H
#define SWIFT_CACHEDDIAGNOSTICS_H

#include "llvm/Support/Error.h"

namespace swift {

class CompilerInstance;
class DiagnosticEngine;
class SourceManager;
class FrontendInputsAndOutputs;

class CachingDiagnosticsProcessor {
public:
  CachingDiagnosticsProcessor(CompilerInstance &Instance);
  ~CachingDiagnosticsProcessor();

  /// Start capturing all the diagnostics from DiagnosticsEngine.
  void startDiagnosticCapture();
  /// End capturing all the diagnostics from DiagnosticsEngine.
  void endDiagnosticCapture();

  /// Emit serialized diagnostics into output stream.
  llvm::Error serializeEmittedDiagnostics(llvm::raw_ostream &os);

  /// Used to replay the previously cached diagnostics, after a cache hit.
  llvm::Error replayCachedDiagnostics(llvm::StringRef Buffer);

private:
  class Implementation;
  Implementation& Impl;
};

}

#endif
