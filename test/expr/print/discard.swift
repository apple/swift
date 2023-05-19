// RUN: %target-swift-frontend -print-ast %s 2>&1 | %FileCheck %s

@_moveOnly
struct S {
  __consuming func c() {
    discard self
  }

  deinit {}
}

// CHECK: @_moveOnly internal struct S {
// CHECK:   internal __consuming func c() {
// CHECK:     discard self
// CHECK:   }
