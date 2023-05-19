// RUN: %target-typecheck-verify-swift -disable-availability-checking

protocol P {}

protocol P1 {
  associatedtype each A // expected-error {{associated types cannot be variadic}}{{18-23=}}
  associatedtype each B<U>
  // expected-error@-1 {{associated types cannot be variadic}}{{18-23=}}
  // expected-error@-2 {{associated types must not have a generic parameter list}}{{24-27=}}
  associatedtype C... // expected-error {{associated types cannot be variadic}}{{19-22=}}
  associatedtype D<U>...
  // expected-error@-1 {{associated types cannot be variadic}}{{22-25=}}
  // expected-error@-2 {{associated types must not have a generic parameter list}}{{19-22=}}
}

typealias Alias<each T> = (repeat each T)

struct S1<each T> {}
struct S2<T, each U> {}
struct S3<each T, U> {}

struct S4<each T:P, U> {}
struct S5<each T :P, U> {}
struct S6<each T: P> {}
struct S7<each T : P> {}

func foo<each T>(_ x: repeat each T) {}

func bar<each T:P>(_ x: repeat each T) {}
func baz<each T :P>(_ x: repeat each T) {}
func qux<each T : P>(_ x: repeat each T) {}
func quux<each T: P>(_ x: repeat each T) {}

func foobar<T, U, each V>(x: T, y: U, z: repeat each V) { }
func foobaz<T, each U, V>(x: T, y: repeat each U, z: V) { }
func fooqux<each T, each U, each V>(x: repeat each T, y: repeat each U, z: repeat each V) { }

func ellipsis<T...>(_ x: repeat each T) {}
// expected-error@-1 {{ellipsis operator cannot be used with a type parameter pack}}{{16-19=}}{{15-15=each }}
// expected-error@-2 {{cannot find type 'T' in scope}}
func eachEllipsis<each T...>(_ x: repeat each T) {}
// expected-error@-1 {{ellipsis operator cannot be used with a type parameter pack}}{{25-28=}}
// expected-error@-2 {{cannot find type 'T' in scope}}
