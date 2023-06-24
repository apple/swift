// RUN: %target-swift-frontend -emit-ir %s | %FileCheck %s

func markUsed<T>(_ t: T) {}

protocol A {
  associatedtype B
  func b(_: B)
}

struct X<Y> : A {
  // CHECK-LABEL: define internal swiftcc void @"$s23dependent_reabstraction1XVyxGAA1AA2aEP1byy1BQzFTW"(ptr noalias nocapture dereferenceable({{.*}}) %0, ptr noalias nocapture swiftself %1, ptr %Self, ptr %SelfWitnessTable)
  func b(_ b: X.Type) {
    let x: Any = b
    markUsed(b as X.Type)
  }
}

func foo<T: A>(_ x: T, _ y: T.B) {
  x.b(y)
}

let a = X<Int>()
let b = X<String>()

foo(a, X<Int>.self)
foo(b, X<String>.self)
