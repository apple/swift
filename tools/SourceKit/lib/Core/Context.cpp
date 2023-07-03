//===--- Context.cpp ------------------------------------------------------===//
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

#include "SourceKit/Core/Context.h"
#include "SourceKit/Core/LangSupport.h"
#include "SourceKit/Core/NotificationCenter.h"

using namespace SourceKit;

GlobalConfig::Settings GlobalConfig::update(
    llvm::Optional<unsigned> CompletionMaxASTContextReuseCount,
    llvm::Optional<unsigned> CompletionCheckDependencyInterval) {
  llvm::sys::ScopedLock L(Mtx);
  if (CompletionMaxASTContextReuseCount.has_value())
    State.IDEInspectionOpts.MaxASTContextReuseCount =
        *CompletionMaxASTContextReuseCount;
  if (CompletionCheckDependencyInterval.has_value())
    State.IDEInspectionOpts.CheckDependencyInterval =
        *CompletionCheckDependencyInterval;
  return State;
}

GlobalConfig::Settings::IDEInspectionOptions
GlobalConfig::getIDEInspectionOpts() const {
  llvm::sys::ScopedLock L(Mtx);
  return State.IDEInspectionOpts;
}

SourceKit::Context::Context(
    StringRef SwiftExecutablePath, StringRef RuntimeLibPath,
    StringRef DiagnosticDocumentationPath,
    llvm::function_ref<std::unique_ptr<LangSupport>(Context &)>
        LangSupportFactoryFn,
    bool shouldDispatchNotificationsOnMain)
    : SwiftExecutablePath(SwiftExecutablePath), RuntimeLibPath(RuntimeLibPath),
      DiagnosticDocumentationPath(DiagnosticDocumentationPath),
      NotificationCtr(
          new NotificationCenter(shouldDispatchNotificationsOnMain)),
      Config(new GlobalConfig()), ReqTracker(new RequestTracker()),
      SlowRequestSim(new SlowRequestSimulator(ReqTracker)) {
  // Should be called last after everything is initialized.
  SwiftLang = LangSupportFactoryFn(*this);
}

SourceKit::Context::~Context() {
}
