// RUN: rm -rf %t
// RUN: split-file %s %t
// RUN: %target-swift-ide-test -print-module -module-to-print=Test -I %t/Inputs -source-filename=x -enable-experimental-cxx-interop | %FileCheck %s

//--- Inputs/module.modulemap
module Test {
    header "test.h"
    requires cplusplus
}

//--- Inputs/test.h
struct A {
    void memberInA(int x);
};

struct B {
    friend void A::memberInA(int);

    void memberInB();
};

struct C: B {
    void memberInC();
};

// CHECK: struct A {
// CHECK:  mutating func memberInA(_ x: Int32)
// CHECK-NEXT: }
// CHECK: struct B {
// CHECK:  mutating func memberInB()
// CHECK-NEXT: }
// CHECK: struct C {
// CHECK:  mutating func memberInC()
// CHECK-NEXT:  mutating func memberInB()
// CHECK-NEXT: }
