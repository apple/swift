// RUN: %target-swift-frontend -typecheck %s -debug-generic-signatures -disable-availability-checking 2>&1 | %FileCheck %s

protocol P {
  associatedtype A
}

// CHECK-LABEL: inferSameShape(ts:us:)
// CHECK-NEXT: Generic signature: <each T, each U where (repeat (each T, each U)) : Any>
func inferSameShape<each T, each U>(ts t: repeat each T, us u: repeat each U) where (repeat (each T, each U)): Any {
}

// CHECK-LABEL: desugarSameShape(ts:us:)
// CHECK-NEXT: Generic signature: <each T, each U where repeat each T : P, (repeat (each T, each U)) : Any, repeat each U : P>
func desugarSameShape<each T, each U>(ts t: repeat each T, us u: repeat each U)
  where repeat each T: P, repeat each U: P, (repeat (each T.A, each U.A)): Any {}

// CHECK-LABEL: multipleSameShape1(ts:us:vs:)
// CHECK-NEXT: Generic signature: <each T, each U, each V where (repeat (each T, each U)) : Any, (repeat (each U, each V)) : Any>
func multipleSameShape1<each T, each U, each V>(ts t: repeat each T, us u: repeat each U, vs v: repeat each V) where (repeat (each T, each U, each V)): Any {
}

// CHECK-LABEL: multipleSameShape2(ts:us:vs:)
// CHECK-NEXT: Generic signature: <each T, each U, each V where (repeat (each T, each U)) : Any, (repeat (each U, each V)) : Any>
func multipleSameShape2<each T, each U, each V>(ts t: repeat each T, us u: repeat each U, vs v: repeat each V) where (repeat (each V, each T, each U)): Any {
}

// CHECK-LABEL: multipleSameShape3(ts:us:vs:)
// CHECK-NEXT: Generic signature: <each T, each U, each V where (repeat (each T, each U)) : Any, (repeat (each U, each V)) : Any>
func multipleSameShape3<each T, each U, each V>(ts t: repeat each T, us u: repeat each U, vs v: repeat each V) where (repeat (each U, each V, each T)): Any {
}

// CHECK-LABEL: multipleSameShape4(ts:us:vs:)
// CHECK-NEXT: Generic signature: <each T, each U, each V where (repeat (each T, each U)) : Any, (repeat (each U, each V)) : Any>
func multipleSameShape4<each T, each U, each V>(ts t: repeat each T, us u: repeat each U, vs v: repeat each V) where (repeat (each U, each T, each V)): Any {
}

// CHECK-LABEL: multipleSameShape5(ts:us:vs:)
// CHECK-NEXT: Generic signature: <each T, each U, each V where (repeat (each T, each U)) : Any, (repeat (each U, each V)) : Any>
func multipleSameShape5<each T, each U, each V>(ts t: repeat each T, us u: repeat each U, vs v: repeat each V) where (repeat (each T, each V, each U)): Any {
}

// CHECK-LABEL: multipleSameShape6(ts:us:vs:)
// CHECK-NEXT: Generic signature: <each T, each U, each V where (repeat (each T, each U)) : Any, (repeat (each U, each V)) : Any>
func multipleSameShape6<each T, each U, each V>(ts t: repeat each T, us u: repeat each U, vs v: repeat each V) where (repeat (each V, each U, each T)): Any {
}

struct Ts<each T> {
  struct Us<each U> {
    // CHECK-LABEL: Ts.Us.packEquality()
    // CHECK-NEXT: Generic signature: <each T, each U where repeat each T == each U>
    func packEquality() where repeat each T == each U, (repeat (each T, each U)): Any {
    }

    struct Vs<each V> {
      // CHECK-LABEL: Ts.Us.Vs.packEquality()
      // CHECK-NEXT: Generic signature: <each T, each U, each V where repeat each T == each U, (repeat (each T, each V)) : Any>
      func packEquality() where repeat each T == each U, (repeat (each U, each V)): Any {
      }
    }
  }
}

// CHECK-LABEL: expandedParameters(_:transform:)
// CHECK-NEXT: Generic signature: <each T, each Result where (repeat (each T, each Result)) : Any>
func expandedParameters<each T, each Result>(_ t: repeat each T, transform: repeat (each T) -> each Result) -> (repeat each Result) {
  fatalError()
}
