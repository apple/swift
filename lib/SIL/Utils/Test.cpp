//===- ParseTestSpecification.h - Parsing for test instructions -*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file defines test::FunctionTest.
//
//===----------------------------------------------------------------------===//

#include "swift/Basic/Assertions.h"
#include "swift/SIL/Test.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace swift;
using namespace swift::test;

namespace {

class Registry {
  StringMap<FunctionTest> registeredTests;
  SwiftNativeFunctionTestThunk thunk;

public:
  static Registry &get() {
    static Registry registry;
    return registry;
  }

  void registerFunctionTest(FunctionTest test, StringRef name) {
    auto inserted = registeredTests.insert({name, test}).second;
    assert(inserted);
    (void)inserted;
  }

  void registerFunctionTestThunk(SwiftNativeFunctionTestThunk thunk) {
    this->thunk = thunk;
  }

  SwiftNativeFunctionTestThunk getFunctionTestThunk() { return thunk; }

  FunctionTest getFunctionTest(StringRef name) {
    auto iter = registeredTests.find(name);
    if (iter == registeredTests.end()) {
      llvm::errs() << "Found no test named " << name << "!\n";
      print(llvm::errs());
    }
    return iter->getValue();
  }

  void print(raw_ostream &OS) const {
    OS << "test::Registry(" << this << ") with " << registeredTests.size()
       << " entries: {{\n";
    for (auto &stringMapEntry : registeredTests) {
      OS << "\t" << stringMapEntry.getKey() << " -> "
         << &stringMapEntry.getValue() << "\n";
    }
    OS << "}} test::Registry(" << this << ")\n";
  }

  void dump() const { print(llvm::dbgs()); }
};

} // end anonymous namespace

void registerFunctionTestThunk(SwiftNativeFunctionTestThunk thunk) {
  Registry::get().registerFunctionTestThunk(thunk);
}

FunctionTest::FunctionTest(StringRef name, Invocation invocation)
    : invocation(invocation), pass(nullptr), function(nullptr),
      dependencies(nullptr) {
  Registry::get().registerFunctionTest(*this, name);
}
FunctionTest::FunctionTest(StringRef name, NativeSwiftInvocation invocation)
    : invocation(invocation), pass(nullptr), function(nullptr),
      dependencies(nullptr) {}

void FunctionTest::createNativeSwiftFunctionTest(
    StringRef name, NativeSwiftInvocation invocation) {
  Registry::get().registerFunctionTest({name, invocation}, name);
}

FunctionTest FunctionTest::get(StringRef name) {
  return Registry::get().getFunctionTest(name);
}

void FunctionTest::run(SILFunction &function, Arguments &arguments,
                       SILFunctionTransform &pass, Dependencies &dependencies) {
  this->pass = &pass;
  this->function = &function;
  this->dependencies = &dependencies;
  if (invocation.isa<Invocation>()) {
    auto fn = invocation.get<Invocation>();
    fn(function, arguments, *this);
  } else {
    llvm::outs().flush();
    auto *fn = invocation.get<NativeSwiftInvocation>();
    Registry::get().getFunctionTestThunk()(fn, {&function}, {&arguments},
                                           {getSwiftPassInvocation()});
    fflush(stdout);
  }
  this->pass = nullptr;
  this->function = nullptr;
  this->dependencies = nullptr;
}

DominanceInfo *FunctionTest::getDominanceInfo() {
  return dependencies->getDominanceInfo();
}

SILPassManager *FunctionTest::getPassManager() {
  return dependencies->getPassManager();
}

SwiftPassInvocation *FunctionTest::getSwiftPassInvocation() {
  return dependencies->getSwiftPassInvocation();
}
