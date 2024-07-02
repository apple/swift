// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %target-interop-build-clangxx -std=c++20 -c %t/use-swift-cxx-types.cpp -I %t -o %t/swift-cxx-execution.o
// RUN: not %target-interop-build-swift %t/use-cxx-types.swift -o %t/swift-cxx-execution -Xlinker %t/swift-cxx-execution.o -module-name UseCxx -Xfrontend -entry-point-function-name -Xfrontend swiftMain -I %t -O -Xfrontend -disable-availability-checking 2> %t/out

// RUN: cat %t/out | %FileCheck %s

//--- header.h

class SharedFRT {
public:
    int x;
} __attribute__((swift_attr("import_reference")))
__attribute__((swift_attr("retain:retainShared")))
__attribute__((swift_attr("release:releaseShared")));

inline void retainShared(SharedFRT *r) { }
inline void releaseShared(SharedFRT *r) { }

//--- use-swift-cxx-types.cpp
#include "header.h"

int main() {}

//--- module.modulemap
module CxxTest {
    header "header.h"
    requires cplusplus
}

//--- use-cxx-types.swift
import CxxTest

public func consumeSharedFRT(_ x: consuming SharedFRT) {}
public func takeSharedFRT(_ x: SharedFRT) {
    consumeSharedFRT(consume x)
    // CHECK: error: 'consume' applied to value that the compiler does not support. This is a compiler bug. Please file a bug with a small example of the bug
    x.x = 10
}
