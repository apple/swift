// RUN: %target-typecheck-verify-swift

//===----------------------------------------------------------------------===//
// Generic function declarations
//===----------------------------------------------------------------------===//

func f0<T>(x: Int, y: Int, t: T) { }
func f1<T : Any>(x: Int, y: Int, t: T) { }
func f2<T : IteratorProtocol>(x: Int, y: Int, t: T) { }
func f3<T : () -> ()>(x: Int, y: Int, t: T) { } // expected-error{{expected a class type or protocol-constrained type restricting 'T'}}
func f4<T>(x: T, y: T) { }

// Non-protocol type constraints.
func f6<T : Wonka>(x: T) {} // expected-error{{cannot find type 'Wonka' in scope}}

func f7<T : Int>(x: T) {} // expected-error{{type 'T' constrained to non-protocol, non-class type 'Int'}}

func f8<T> (x: Int) {} //expected-error{{generic parameter 'T' is not used in function signature}}

public class A<X> {
  init<T>(){} //expected-error{{generic parameter 'T' is not used in function signature}}
  public func f9<T, U>(x: T, y: X) {} //expected-error{{generic parameter 'U' is not used in function signature}}
  public func f10(x: Int) {}
  public func f11<T, U>(x: X, y: T) {} //expected-error{{generic parameter 'U' is not used in function signature}}
}

struct G<T> {}

struct GG<T, U> {}

protocol P {
  associatedtype A
  typealias B = Int
  typealias C<T> = T
  typealias D = G // expected-note {{generic type 'D' declared here}}
  typealias E<T> = GG<T, T>
}

func f12<T : P>(x: T) -> T.A<Int> {} // expected-error {{cannot specialize non-generic type 'T.A'}}{{29-34=}}

func f13<T : P>(x: T) -> T.B<Int> {} // expected-error {{cannot specialize non-generic type 'T.B' (aka 'Int')}}{{29-34=}}

func f14<T : P>(x: T) -> T.C<Int> {}

func f15<T : P>(x: T) -> T.D<Int> {}

func f16<T : P>(x: T) -> T.D {} // expected-error {{reference to generic type 'T.D' requires arguments in <...>}}

func f17<T : P>(x: T) -> T.E<Int> {}

public protocol Q {
    associatedtype AT1
    associatedtype AT2
}

public protocol R {
  associatedtype I
}

public class C {
}

// Check that all generic params except for V can be inferred, because
// they are directly or indirectly used to declare function's arguments.
// In particular, T and U are considered to be used, because E is explicitly
// used and there is a requirement E.I == (T, U)
//expected-error@+1{{generic parameter 'V' is not used in function signature}}
public func expectEqualSequence<E : R, A : R, T : Q, U : Q, V : Q>(
  _ expected: E, _ actual: A
) where
  E.I == A.I,
  E.I == (T, U) {
}

// Check that all generic params can be inferred, because
// they are directly or indirectly used to declare function's arguments.
// The only generic param that is used explicitly is T6.
// All others are inferred from it step-by-step through the chain of
// requirements.
func foo<T1:Q, T2:Q, T3:Q, T4:Q, T5:Q, T6:Q> (x: T6)
  where T1.AT1 == T2.AT1, T1 == T6.AT2, T2 == T3.AT1, T3 == T4.AT1, T4 == T5.AT2, T5 == T6.AT1 {
}

// Check that all generic params except for T1 and T2 can be inferred, because
// they are directly or indirectly used to declare function's arguments.
//expected-error@+2{{generic parameter 'T1' is not used in function signature}}
//expected-error@+1{{generic parameter 'T2' is not used in function signature}}
func boo<T1, T2:Q, T3:Q, T4:Q, T5:Q, T6:Q> (x: T6)
  where T1:Q, T2:C, T3 == T4.AT1, T4 == T5.AT2, T5 == T6.AT1 {
}

// At the first glance U seems to be explicitly used for declaring a parameter d.
// But in fact, the declaration of d uses the associated type U.A. This is not
// enough to infer the type of U at any of the baz's call-sites.
// Therefore we should reject this declaration.
//expected-error@+1{{generic parameter 'U' is not used in function signature}}
func baz<U:P>(_ d: U.A) {
}
