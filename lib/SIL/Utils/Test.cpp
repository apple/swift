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

#include "swift/SIL/Test.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace swift;
using namespace swift::test;

namespace {

class Registry {
  DenseMap<StringRef, FunctionTest *> registeredTests;

public:
  static Registry &get() {
    static Registry registry;
    return registry;
  }

  void registerFunctionTest(FunctionTest *test, StringRef name) {
    auto inserted = registeredTests.insert({name, test}).second;
    assert(inserted);
    (void)inserted;
  }

  FunctionTest *getFunctionTest(StringRef name) {
    auto *res = registeredTests[name];
    if (!res) {
      llvm::errs() << "Found no test named " << name << "!\n";
      print(llvm::errs());
    }
    return res;
  }

  void print(raw_ostream &OS) const {
    OS << "test::Registry(" << this << ") with " << registeredTests.size()
       << " entries: {{\n";
    for (auto pair : registeredTests) {
      OS << "\t" << pair.getFirst() << " -> " << pair.getSecond() << "\n";
    }
    OS << "}} test::Registry(" << this << ")\n";
  }

  void dump() const { print(llvm::dbgs()); }
};

} // end anonymous namespace

FunctionTest::FunctionTest(StringRef name, Invocation invocation)
    : invocation(invocation), pass(nullptr), function(nullptr),
      dependencies(nullptr) {
  Registry::get().registerFunctionTest(this, name);
}

FunctionTest *FunctionTest::get(StringRef name) {
  return Registry::get().getFunctionTest(name);
}

void FunctionTest::run(SILFunction &function, Arguments &arguments,
                       SILFunctionTransform &pass, Dependencies &dependencies) {
  this->pass = &pass;
  this->function = &function;
  this->dependencies = &dependencies;
  invocation(function, arguments, *this);
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
