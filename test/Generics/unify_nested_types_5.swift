// RUN: %target-typecheck-verify-swift -enable-requirement-machine

protocol P {
  associatedtype A : P
}

class C<U> : P {
  typealias A = C<U>
}

protocol P1 {
  associatedtype T : P
}

protocol P2 {
  associatedtype T where T : C<U>
  associatedtype U
}

func eq<T>(_: T, _: T) {}

struct G<T : P1 & P2> {
  func foo(_ x: T.T.A, _ y: C<T.U>) {
    eq(x, y)
  }
}