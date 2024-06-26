// Tests that under -enable-llvm-vfe, a base method implementation that is provably
// never called is safely removed

// REQUIRES: executable_test
// REQUIRES: VENDOR=apple
// REQUIRES: no_asan
// UNSUPPORTED: remote_run

// RUN: %empty-directory(%t)

// (1) Build the program
// RUN: %target-build-swift -Xfrontend -enable-llvm-vfe -lto=llvm-full %lto_flags -o %t/main %s

// (2) Check that Base.foo, Derived.foo, and Derived.bar are preserved but Base.bar is not
// RUN: %llvm-nm --defined-only %t/main | %FileCheck %s --check-prefix=NM

// (3) Run the program
// RUN: %target-run %t/main | %FileCheck %s

class Base {
    func foo() { print("Base.foo") }
    func bar() { print("Base.bar") }
}

class Derived : Base {
    override func foo() { print("Derived.foo") }
    override func bar() { print("Derived.bar") }
}

let base = Base()
let typeErasedDerived: Base = Derived()
let derived = Derived()

// NM-NOT: $s4main4BaseC3baryyF 
// NM:     $s4main4BaseC3fooyyF
// NM:     $s4main7DerivedC3baryyF
// NM:     $s4main7DerivedC3fooyyF

base.foo()
// CHECK: Base.foo
typeErasedDerived.foo()
// CHECK: Derived.foo
derived.bar()
// CHECK: Derived.bar

