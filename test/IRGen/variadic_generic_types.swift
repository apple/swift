// RUN: %target-swift-frontend -emit-ir -primary-file %s -disable-availability-checking | %FileCheck %s

// REQUIRES: PTRSIZE=64

struct G<each T> {
  // CHECK-LABEL: define hidden swiftcc void @"$s22variadic_generic_types1GV6calleeyyF"(i64 %0, %swift.type** %"each T")
  // CHECK: ret void
  func callee() {}

  // CHECK-LABEL: define hidden swiftcc void @"$s22variadic_generic_types1GV6calleryyF"(i64 %0, %swift.type** %"each T")
  // CHECK: call swiftcc void @"$s22variadic_generic_types1GV6calleeyyF"(i64 %0, %swift.type** %"each T")
  // CHECK: ret void
  func caller() {
    callee()
  }

  func makeTuple1() -> (repeat each T).Type {
    return (repeat each T).self
  }

  func makeTuple2() -> (repeat Array<each T>).Type {
    return (repeat Array<each T>).self
  }
}

func blackHole<T>(_: T) {}

blackHole(G< >.self)
blackHole(G<Int, String>.self)

let g = G<Int, String, Float>()
blackHole(g.makeTuple1())
blackHole(g.makeTuple2())
