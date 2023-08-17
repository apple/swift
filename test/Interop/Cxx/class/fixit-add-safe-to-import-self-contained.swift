// RUN: rm -rf %t
// RUN: split-file %s %t
// RUN: not %target-swift-frontend -typecheck -I %t/Inputs  %t/test.swift  -enable-experimental-cxx-interop 2>&1 | %FileCheck %s

//--- Inputs/module.modulemap
module Test {
    header "test.h"
    requires cplusplus
}

//--- Inputs/test.h
struct Ptr { int *p; };

struct X {
  X(const X&);
  
  int *test() { }
  Ptr other() { }
};

//--- test.swift

import Test

public func test(x: X) {
  // CHECK: note: annotate method 'test' with 'SWIFT_RETURNS_INDEPENDENT_VALUE' in C++ to make it available in Swift
  // CHECK: int *test() { }
  // CHECK: ^
  // CHECK: SWIFT_RETURNS_INDEPENDENT_VALUE
  
  x.test()
  
  // CHECK: note: annotate method 'other' with 'SWIFT_RETURNS_INDEPENDENT_VALUE' in C++ to make it available in Swift
  // CHECK: Ptr other() { }
  // CHECK: ^
  // CHECK: SWIFT_RETURNS_INDEPENDENT_VALUE
  
  // CHECK: note: annotate type 'Ptr' with 'SWIFT_SELF_CONTAINED' in C++ to make methods that return it available in Swift
  // CHECK: struct Ptr {
  // CHECK: ^
  // CHECK: SWIFT_SELF_CONTAINED
  x.other()
}
