// RUN: %target-typecheck-verify-swift

class HasFunc {
  func HasFunc(_: HasFunc) {
  }
  func HasFunc() -> HasFunc {
    return HasFunc()
  }
  func SomethingElse(_: SomethingElse) { // expected-error {{cannot find type 'SomethingElse' in scope}}
    return nil // expected-error {{unexpected non-void return value in void function}}
  }
  func SomethingElse() -> SomethingElse? { // expected-error {{cannot find type 'SomethingElse' in scope}}
    return nil
  }
}

class HasGenericFunc {
  func HasGenericFunc<HasGenericFunc : HasGenericFunc>(x: HasGenericFunc) -> HasGenericFunc { // expected-error {{type 'HasGenericFunc' constrained to non-protocol, non-class type 'HasGenericFunc'}}
    return x
  }
  func SomethingElse<SomethingElse : SomethingElse>(_: SomethingElse) -> SomethingElse? { // expected-error {{type 'SomethingElse' constrained to non-protocol, non-class type 'SomethingElse'}}
    return nil
  }
}

class HasProp { // expected-note {{'HasProp' declared here}}
  var HasProp: HasProp {
    return HasProp() // expected-error {{use of 'HasProp' refers to instance method rather than class 'HasProp' in module 'circular_decl_checking'}}
    // expected-note@-1 {{use 'circular_decl_checking.' to reference the class in module 'circular_decl_checking'}} {{12-12=circular_decl_checking.}}
  }
  var SomethingElse: SomethingElse? { // expected-error {{cannot find type 'SomethingElse' in scope}}
    return nil
  }
}

protocol SomeProtocol {}
protocol ReferenceSomeProtocol {
  var SomeProtocol: SomeProtocol { get } 
}

func TopLevelFunc(x: TopLevelFunc) -> TopLevelFunc { return x } // expected-error 2 {{cannot find type 'TopLevelFunc' in scope}}'
func TopLevelGenericFunc<TopLevelGenericFunc : TopLevelGenericFunc>(x: TopLevelGenericFunc) -> TopLevelGenericFunc { return x } // expected-error {{type 'TopLevelGenericFunc' constrained to non-protocol, non-class type 'TopLevelGenericFunc'}}
func TopLevelGenericFunc2<T : TopLevelGenericFunc2>(x: T) -> T { return x} // expected-error {{cannot find type 'TopLevelGenericFunc2' in scope}}
var TopLevelVar: TopLevelVar? { return nil } // expected-error {{cannot find type 'TopLevelVar' in scope}}


protocol AProtocol {
  associatedtype e : e
  // expected-error@-1 {{type 'Self.e' constrained to non-protocol, non-class type 'Self.e'}}
}



// <rdar://problem/15604574> Protocol conformance checking needs to be delayed
protocol P15604574 {
  associatedtype FooResult
  func foo() -> FooResult
}

class AcceptsP<T : P15604574> { }

class X {
  func foo() -> AcceptsP<X> { } // expected-error {{type 'X' does not conform to protocol 'P15604574'}}
}

// <rdar://problem/17144076> recursive typealias causes a segfault in the type checker
struct SomeStruct<A> {
  typealias A = A // this is OK now -- the underlying type is the generic parameter 'A'
  typealias B = B // expected-error {{type alias 'B' references itself}} expected-note {{while resolving type 'B'}}
  // expected-error@-1 {{circular reference}}
}

// <rdar://problem/27680407> Infinite recursion when using fully-qualified associatedtype name that has not been defined with typealias
protocol rdar27680407Proto {
  associatedtype T // expected-note {{protocol requires nested type 'T'; do you want to add it?}}

  init(value: T)
}

struct rdar27680407Struct : rdar27680407Proto { // expected-error {{type 'rdar27680407Struct' does not conform to protocol 'rdar27680407Proto'}}
  init(value: rdar27680407Struct.T) {}
}
