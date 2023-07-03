//===--- Task.inc - ---------------------------------------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file contains the default implementation of the class Task
///
//===----------------------------------------------------------------------===//

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Optional.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/Signals.h"

namespace swift {
namespace sys {

// Platform-independent implementation of Task,
// a particular platform can provide its own more efficient version.
class Task {
public:
  /// The path to the executable which this Task will execute.
  const char *ExecPath;

  /// Any arguments which should be passed during execution.
  llvm::ArrayRef<const char *> Args;

  /// The environment which should be used during execution. If empty,
  /// the current process's environment will be used instead.
  llvm::ArrayRef<const char *> Env;

  /// Context associated with this Task.
  void *Context;

  /// True if the errors of the Task should be stored in Errors instead of
  /// Output.
  bool SeparateErrors;

  llvm::SmallString<64> StdoutPath;

  llvm::SmallString<64> StderrPath;

  llvm::sys::ProcessInfo PI;

  Task(const char *ExecPath, llvm::ArrayRef<const char *> Args,
       llvm::ArrayRef<const char *> Env = llvm::None, void *Context = nullptr,
       bool SeparateErrors = false)
      : ExecPath(ExecPath), Args(Args), Env(Env), Context(Context),
        SeparateErrors(SeparateErrors) {}

  /// Begins execution of this Task.
  /// \returns true on error.
  bool execute();
};

bool Task::execute() {
  llvm::SmallVector<const char *, 128> Argv;
  Argv.push_back(ExecPath);
  Argv.append(Args.begin(), Args.end());
  Argv.push_back(nullptr);

  llvm::Optional<llvm::ArrayRef<llvm::StringRef>> Envp =
      Env.empty() ? decltype(Envp)(llvm::None)
                  : decltype(Envp)(llvm::toStringRefArray(Env.data()));

  if (llvm::sys::fs::createTemporaryFile("stdout", "tmp", StdoutPath))
    return true;
  llvm::sys::RemoveFileOnSignal(StdoutPath);

  if (SeparateErrors) {
    if (llvm::sys::fs::createTemporaryFile("stderr", "tmp", StderrPath))
      return true;
    llvm::sys::RemoveFileOnSignal(StderrPath);
  }

  llvm::Optional<llvm::StringRef> Redirects[] = {
    llvm::None,
      llvm::StringRef{StdoutPath},
      llvm::StringRef{SeparateErrors ? StderrPath : StdoutPath},
  };

  bool ExecutionFailed = false;
  PI = llvm::sys::ExecuteNoWait(ExecPath, llvm::toStringRefArray(Argv.data()),
                                Envp, Redirects, /*memoryLimit*/ 0,
                                /*ErrMsg*/ nullptr, &ExecutionFailed);
  return ExecutionFailed;
}

} // namespace sys
} // namespace swift
