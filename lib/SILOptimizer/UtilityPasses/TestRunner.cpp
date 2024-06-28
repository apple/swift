//===-------------------------- TestRunner.cpp ----------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file defines test::TestRunner, the pass responsible for running tests,
// specifically test::FunctionTest and test::ModuleTest (maybe someday).
//
// To see more about writing your own tests, see include/swift/SIL/Test.h.
//
//===----------------------------------------------------------------------===//

#include "swift/Basic/Assertions.h"
#include "swift/SIL/Test.h"
#include "swift/SILOptimizer/Analysis/DominanceAnalysis.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"

using namespace swift;
using namespace swift::test;

namespace swift::test {
class TestRunner : public SILFunctionTransform {
  void printTestLifetime(bool begin, unsigned testIndex, unsigned testCount,
                         StringRef name, ArrayRef<StringRef> components);
  void runTest(StringRef name, Arguments &arguments);
  void run() override;
  struct FunctionTestDependenciesImpl final
      : public test::FunctionTest::Dependencies {
    TestRunner *pass;
    SILFunction *function;
    SwiftPassInvocation swiftPassInvocation;
    FunctionTestDependenciesImpl(TestRunner *pass, SILFunction *function)
        : pass(pass), function(function),
          swiftPassInvocation(pass->getPassManager(), pass, function) {}
    DominanceInfo *getDominanceInfo() override {
      auto *dominanceAnalysis = pass->getAnalysis<DominanceAnalysis>();
      return dominanceAnalysis->get(function);
    }
    SwiftPassInvocation *getSwiftPassInvocation() override {
      return &swiftPassInvocation;
    }
    SILPassManager *getPassManager() override { return pass->getPassManager(); }
    ~FunctionTestDependenciesImpl() {}
  };
};

void TestRunner::printTestLifetime(bool begin, unsigned testIndex,
                                   unsigned testCount, StringRef name,
                                   ArrayRef<StringRef> components) {
  StringRef word = begin ? "\nbegin" : "end";
  llvm::outs() << word << " running test " << testIndex + 1 << " of "
               << testCount << " on " << getFunction()->getName() << ": "
               << name << " with: ";
  for (unsigned long index = 0, size = components.size(); index < size;
       ++index) {
    auto componentString = components[index].trim();
    if (componentString.empty())
      continue;

    llvm::outs() << componentString;
    if (index != size - 1) {
      llvm::outs() << ", ";
    }
  }
  llvm::outs() << "\n";
}

void TestRunner::runTest(StringRef name, Arguments &arguments) {
  FunctionTest test = FunctionTest::get(name);
  auto *function = getFunction();
  FunctionTestDependenciesImpl dependencies(this, function);
  test.run(*function, arguments, *this, dependencies);
}

void TestRunner::run() {
  llvm::SmallVector<UnparsedSpecification, 2> testSpecifications;
  getTestSpecifications(getFunction(), testSpecifications);
  Arguments arguments;
  SmallVector<StringRef, 4> components;
  for (unsigned long index = 0, size = testSpecifications.size(); index < size;
       ++index) {
    components.clear();
    arguments.clear();
    auto testSpecification = testSpecifications[index];
    test::parseTestArgumentsFromSpecification(getFunction(), testSpecification,
                                              arguments, components);
    auto name = arguments.takeString();
    ArrayRef<StringRef> argumentStrings = components;
    argumentStrings = argumentStrings.drop_front();
    printTestLifetime(/*begin=*/true, /*index=*/index, /*size=*/size, name,
                      argumentStrings);
    runTest(name, arguments);
    printTestLifetime(/*begin=*/false, /*index=*/index, /*size=*/size, name,
                      argumentStrings);
  }
}

//===----------------------------------------------------------------------===//
// MARK: General Unit Tests
//===----------------------------------------------------------------------===//

// Arguments: NONE
// Dumps:
// - the function
static FunctionTest DumpFunctionTest("dump-function",
                                     [](auto &function, auto &, auto &) {
                                       function.print(llvm::outs());
                                     });

} // namespace swift::test

//===----------------------------------------------------------------------===//
//                           Top Level Entry Point
//===----------------------------------------------------------------------===//

SILTransform *swift::createUnitTestRunner() { return new TestRunner(); }
