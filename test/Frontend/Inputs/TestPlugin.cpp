//===---------------------- TestPlugin.cpp --------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

// Used by -load-pass-plugin

#include "llvm/Pass.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"

using namespace llvm;

namespace {

void runTestPlugin(Function &F) {
  errs() << "TestPlugin: ";
  errs().write_escaped(F.getName()) << '\n';
}

struct TestPluginPass : PassInfoMixin<TestPluginPass> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &) {
    runTestPlugin(F);
    return PreservedAnalyses::all();
  }
};

} // namespace

PassPluginLibraryInfo getTestPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "TestPlugin", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerVectorizerStartEPCallback(
                [](llvm::FunctionPassManager &PM, OptimizationLevel Level) {
                  PM.addPass(TestPluginPass());
            });
          }};
}

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return getTestPluginInfo();
}
