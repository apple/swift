// RUN: %empty-directory(%t)
// RUN: not %target-swift-frontend -emit-sil -verify -serialize-diagnostics-path %t/serialized.dia -emit-fixits-path %t/fixits %s 2>&1 | %FileCheck %s
// RUN: not %target-swift-frontend -emit-sil -verify -warnings-as-errors %s 2>&1 | %FileCheck %s -check-prefix CHECK-WARNINGS-AS-ERRORS

func b() {
  let c = 2
}
// CHECK: unexpected warning produced: initialization of immutable value 'c' was never used
// CHECK-WARNINGS-AS-ERRORS: unexpected error produced: initialization of immutable value 'c' was never used
