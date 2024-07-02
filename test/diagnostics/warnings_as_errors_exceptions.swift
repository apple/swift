// RUN: %target-swift-frontend -typecheck -warnings-as-errors -no-warning-as-error pbd_never_used -no-warning-as-error isa_is_always_true %s 2>&1 | %FileCheck %s --check-prefix=CHECK-WARNING
// RUN: not %target-swift-frontend -typecheck -warnings-as-errors  %s 2>&1 | %FileCheck %s --check-prefix=CHECK-ERROR


// CHECK-WARNING-NOT: error: initialization of immutable value 'x' was never used;
// CHECK-WARNING: warning: initialization of immutable value 'x' was never used;
// CHECK-ERROR-NOT: warning: initialization of immutable value 'x' was never used;
// CHECK-ERROR: error: initialization of immutable value 'x' was never used;
func f() {
  let x = 2
}

// CHECK-WARNING-NOT: error: 'is' test is always true
// CHECK-WARNING: warning: 'is' test is always true
// CHECK-ERROR-NOT: warning: 'is' test is always true
// CHECK-ERROR: error: 'is' test is always true
func g() {
  if 0 is Int {}
}
