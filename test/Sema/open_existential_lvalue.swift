// RUN: %target-typecheck-verify-swift

protocol Q {}

protocol P {
  associatedtype T: Q
  var member: T { get set }
  var otherMember: any Q { get set }
  subscript(_: Int) -> T { get set }
}

func takesAny(x: any Q) {}
func takesRValue(x: any Q) {}
func takesInOut(x: inout any Q) {}

func f(data: inout any P) {
  takesAny(x: data.member)
  takesAny(x: data[0])

  takesRValue(x: data.member)
  takesRValue(x: data[0])

  takesInOut(x: &data.member) // expected-error {{cannot pass immutable value as inout argument: 'data' is immutable}}
  takesInOut(x: &data[0]) // expected-error {{cannot pass immutable value as inout argument: 'data' is immutable}}

  takesInOut(x: &data.otherMember) // okay
}

struct S {
  subscript<T: Q>(_ ct: T.Type) -> T {
    get { fatalError() }
    set { fatalError() }
  }
}

func f(s: inout S, t: any Q.Type) -> (any Q) {
  takesAny(x: s[t])
  takesRValue(x: s[t])
  takesInOut(x: &s[t]) // expected-error {{cannot pass immutable value as inout argument: 's' is immutable}}
}
