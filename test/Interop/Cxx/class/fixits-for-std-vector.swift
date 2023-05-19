// RUN: rm -rf %t
// RUN: split-file %s %t
// RUN: not %target-swift-frontend -typecheck -I %t/Inputs  %t/test.swift  -enable-experimental-cxx-interop 2>&1 | %FileCheck %s

// REQUIRES: OS=macosx || OS=linux-gnu

//--- Inputs/module.modulemap
module Test {
    header "test.h"
    requires cplusplus
}

//--- Inputs/test.h
#include <vector>

using V = std::vector<int>;

//--- test.swift

import Test
import CxxStdlib

public func test(v: V) {
  // CHECK: note: C++ method 'begin' that returns an iterator is unavailable
  // CHECK: note: do you want to use a for-in loop instead?
  // CHECK: ^~~~~
  // CHECK: makeIterator
  _ = v.begin()
  
  // CHECK: note: C++ method 'end' that returns an iterator is unavailable
  // CHECK: note: do you want to compare against 'nil' instead?
  // CHECK: ^~~~
  // CHECK: nil
  _ = v.end()

  // CHECK: note: C++ method 'front' that returns a reference of type 'UnsafePointer' is unavailable
  // CHECK: note: C++ method 'front' may return an interior pointer
  // CHECK: note: do you want to get the first element instead?
  // CHECK: ^~~~~~~
  // CHECK: first
  _ = v.front()
  
  // CHECK: note: C++ method 'back' that returns a reference of type 'UnsafePointer' is unavailable
  // CHECK: note: C++ method 'back' may return an interior pointer
  // CHECK: note: do you want to get the last element instead?
  // CHECK: ^~~~~~
  // CHECK: last
  _ = v.back()
}
