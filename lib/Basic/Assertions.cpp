//===--- Assertions.cpp - Assertion macros --------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2024 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file defines implementation details of include/swift/Basic/Assertions.h.
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include "swift/Basic/Assertions.h"
#undef NDEBUG
#include <cassert>
#include <iostream>

llvm::cl::opt<bool> AssertContinue(
    "assert-continue", llvm::cl::init(false),
    llvm::cl::desc("Do not stop on an assertion failure"));

llvm::cl::opt<bool> AssertHelp(
    "assert-help", llvm::cl::init(false),
    llvm::cl::desc("Print help for managing assertions"));

int CONDITIONAL_ASSERT_Global_enable_flag =
#ifdef NDEBUG
  0; // Default to `off` in release builds
#else
  0; // TODO: Default to `on` in debug builds
#endif

void ASSERT_failure(const char *expr, const char *filename, int line, const char *func) {
  // Format here matches that used by `assert` on macOS:
  llvm::errs()
    << "Assertion failed: "
    << "(" << expr << "), "
    << "function " << func << " at "
    << filename << ":"
    << line << ".\n";

  ASSERT_help();

  if (AssertContinue) {
    llvm::errs() << "Continuing after failed assertion (-Xllvm -assert-continue)\n";
    return;
  }

  abort();
}

void ASSERT_help() {
  static int ASSERT_help_shown = 0;
  if (ASSERT_help_shown) {
    return;
  }
  ASSERT_help_shown = 1;

  if (!AssertHelp) {
    llvm::errs() << "(to display assertion configuration options: -Xllvm -assert-help)\n";
    return;
  }

  llvm::errs() << "\n";
  llvm::errs() << "Control assertion behavior with one or more of the following options:\n\n";
  llvm::errs() << " -Xllvm -assert-continue\n";
  llvm::errs() << "     Continue after any failed assertion\n\n";
}

// This has to be callable in the same way as the macro version,
// so we can't put it inside a namespace.
#undef CONDITIONAL_ASSERT_enabled
int CONDITIONAL_ASSERT_enabled() {
  return (CONDITIONAL_ASSERT_Global_enable_flag != 0);
}
