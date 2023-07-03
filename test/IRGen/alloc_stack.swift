// RUN: %target-swift-frontend -primary-file %s -emit-ir | %FileCheck %s

// REQUIRES: CPU=x86_64

class Foobar {
  init() {
    var a : Bool = true
  }
}

// Make sure we are mis-initializing the alloca.
// CHECK-LABEL: define {{.*}}swiftcc ptr @"$s11alloc_stack6FoobarCACycfc"(ptr swiftself %0)
// CHECK: alloca %TSb, align 1
// CHECK-NOT: store{{.*}}opaque
// CHECK:  ret {{.*}}%0
// CHECK:}

