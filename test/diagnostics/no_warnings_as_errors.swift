// RUN: %target-swift-frontend -emit-sil -warnings-as-errors -no-warnings-as-errors %s 2>&1 | %FileCheck %s --check-prefix=CHECK-WARNING
// RUN: not %target-swift-frontend -emit-sil -no-warnings-as-errors -warnings-as-errors  %s 2>&1 | %FileCheck %s --check-prefix=CHECK-ERROR


// This test verifies that the -no-warnings-as-errors option nullifies the effect of the -warnings-as-errors option
// CHECK-WARNING-NOT: error: initialization of immutable value 'c' was never used;
// CHECK-WARNING: warning: initialization of immutable value 'c' was never used;
// CHECK-ERROR-NOT: warning: initialization of immutable value 'c' was never used;
// CHECK-ERROR: error: initialization of immutable value 'c' was never used;
func b() {
  let c = 2
}
