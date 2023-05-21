// RUN: %target-swift-frontend %s -disable-availability-checking -emit-ir | %FileCheck %s

// REQUIRES: concurrency

// CHECK: define {{.*}}@callee
@_silgen_name("callee")
func callee<each T : P>(_ ts: repeat each T) async {
  (repeat (each ts).foo())
}

// CHECK: define {{.*}}@caller
@_silgen_name("caller")
func caller<each T1 : P>(t1: repeat each T1) async {
  await callee(S(), repeat each t1)
}

struct S : P {
  func foo() {
  }
}
protocol P {
  func foo()
}
