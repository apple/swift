// RUN: %empty-directory(%t)
// RUN: %target-build-swift -module-name main -Xfrontend -unavailable-decl-optimization=stub %s -o %t/a.out
// RUN: %target-codesign %t/a.out
// RUN: %target-run %t/a.out > %t/output 2>&1 || true
// RUN: %FileCheck %s < %t/output

// REQUIRES: executable_test

@available(*, unavailable)
public func foo() {
  print("Can't call this")
}

// To bypass the typechecker, forward declare an available function with the
// same mangling as foo.
@_silgen_name("$s4main3fooyyF")
func callFoo()

// CHECK: Fatal error: Unavailable code reached
// CHECK-NOT: Can't call this
callFoo()
