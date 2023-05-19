// RUN: rm -rf %t
// RUN: split-file %s %t
// RUN: %target-swift-frontend -typecheck -verify -I %t/Inputs  %t/test.swift  -enable-experimental-cxx-interop
// RUN: not %target-swift-frontend -typecheck -I %t/Inputs  %t/test.swift  -enable-experimental-cxx-interop 2>&1 | %FileCheck %s

//--- Inputs/module.modulemap
module Test {
    header "test.h"
    requires cplusplus
}

//--- Inputs/test.h

void acceptRValueRef(int &&);

template<class T>
void notStdMove(T &&);

//--- test.swift

import Test

public func test() {
  var x: CInt = 2
  acceptRValueRef(x) // expected-error {{cannot find 'acceptRValueRef' in scope}}
  // CHECK: note: function 'acceptRValueRef' unavailable (cannot import)
  // CHECK: note: C++ functions with rvalue reference parameters are unavailable in Swift

  notStdMove(x) // expected-error {{cannot find 'notStdMove' in scope}}
  // CHECK: note: function 'notStdMove' unavailable (cannot import)
  // CHECK: note: C++ functions with rvalue reference parameters are unavailable in Swift
}
