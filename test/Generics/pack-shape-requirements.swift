// RUN: %target-swift-frontend -typecheck %s -debug-generic-signatures -disable-availability-checking 2>&1 | %FileCheck %s

protocol P {
  associatedtype A: P
}

// CHECK-LABEL: inferSameShape(ts:us:)
// CHECK-NEXT: Generic signature: <each T, each U where (repeat (each T, each U)) : Any>
func inferSameShape<each T, each U>(ts t: repeat each T, us u: repeat each U) where (repeat (each T, each U)): Any {
}

// CHECK-LABEL: desugarSameShape(ts:us:)
// CHECK-NEXT: Generic signature: <each T, each U where repeat each T : P, (repeat (each T, each U)) : Any, repeat each U : P>
func desugarSameShape<each T, each U>(ts t: repeat each T, us u: repeat each U)
  where repeat each T: P, repeat each U: P, (repeat ((each T).A, (each U).A)): Any {}

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


//////
///
/// Same-type requirements should imply same-shape requirements.
///
//////

// CHECK-LABEL: sameType1
// CHECK-NEXT: Generic signature: <each T, each U where repeat each T : P, repeat each U : P, repeat (each T).[P]A == (each U).[P]A>
func sameType1<each T, each U>(_: repeat (each T, each U)) where repeat each T: P, repeat each U: P, repeat (each T).A == (each U).A {}

// Make sure inherited associated types are handled
protocol Q: P where A: Q {}

// CHECK-LABEL: sameType2
// CHECK-NEXT: Generic signature: <each T, each U where repeat each T : Q, repeat each U : Q, repeat (each T).[P]A.[P]A == (each U).[P]A.[P]A>
func sameType2<each T, each U>(_: repeat (each T, each U)) where repeat each T: Q, repeat each U: Q, repeat (each T).A.A == (each U).A.A {}


//////
///
/// A same-type requirement between two pack expansion types
/// should desugar to a same-shape requirement between their
/// count types and a same-type requirement between their
/// element types.
///
//////

typealias First<T, U> = T
typealias Shape<each T> = (repeat First<(), each T>)

// CHECK-LABEL: sameTypeDesugar1
// CHECK-NEXT: Generic signature: <each T, each U where (repeat (each T, each U)) : Any>
func sameTypeDesugar1<each T, each U>(t: repeat each T, u: repeat each U)
  where Shape<repeat each T> == Shape<repeat each U> {}

// CHECK-LABEL: sameTypeDesugar2
// CHECK-NEXT: Generic signature: <each T, each U where repeat each T : P, (repeat (each T, each U)) : Any, repeat each U : P>
func sameTypeDesugar2<each T: P, each U: P>(t: repeat each T, u: repeat each U)
  where Shape<repeat (each T).A> == Shape<repeat (each U).A> {}

/// More complex example involving concrete type matching in
/// property map construction

protocol PP {
  associatedtype A
}

struct G<each T> {}

// CHECK-LABEL: sameTypeMatch1
// CHECK-NEXT: <T, each U, each V where T : PP, repeat each U : PP, repeat each V : PP, T.[PP]A == G<repeat (each U).[PP]A>, repeat (each U).[PP]A == (each V).[PP]A>
func sameTypeMatch1<T: PP, each U: PP, each V: PP>(t: T, u: repeat each U, v: repeat each V)
  where T.A == G<repeat (each U).A>, T.A == G<repeat (each V).A>,
        (repeat (each U, each V)) : Any {}

// CHECK-LABEL: sameTypeMatch2
// CHECK-NEXT: <T, each U, each V where T : PP, repeat each U : PP, (repeat (each U, each V)) : Any, repeat each V : PP, T.[PP]A == (/* shape: each U */ repeat ())>
func sameTypeMatch2<T: PP, each U: PP, each V: PP>(t: T, u: repeat each U, v: repeat each V)
  where T.A == Shape<repeat each U>, T.A == Shape<repeat each V> {}