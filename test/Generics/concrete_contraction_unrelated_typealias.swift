// RUN: %target-swift-frontend -typecheck -verify %s -debug-generic-signatures -warn-redundant-requirements 2>&1 | %FileCheck %s

// Another GenericSignatureBuilder oddity, reduced from RxSwift.
//
// The requirements 'Proxy.Parent == P' and 'Proxy.Delegate == D' in the
// init() below refer to both the typealias and the associated type,
// despite the class being unrelated to the protocol; it just happens to
// define typealiases with the same name.
//
// In the Requirement Machine, the concrete contraction pre-processing
// pass would eagerly substitute the concrete type into these two
// requirements, producing the useless requirements 'P == P' and 'D == D'.
//
// Make sure concrete contraction keeps these requirements as-is by
// checking the generic signature with and without concrete contraction.

class GenericDelegateProxy<P : AnyObject, D> {
  typealias Parent = P
  typealias Delegate = D

  // Here if we resolve Proxy.Parent and Proxy.Delegate to the typealiases,
  // we get vacuous requirements 'P == P' and 'D == D'. By keeping both
  // the substituted and original requirement, we ensure that the
  // unrelated associated type 'Parent' is constrained instead.

  // CHECK-LABEL: .GenericDelegateProxy.init(_:)@
  // CHECK-NEXT: <P, D, Proxy where P == Proxy.[DelegateProxyType]Parent, D == Proxy.[DelegateProxyType]Delegate, Proxy : GenericDelegateProxy<P, D>, Proxy : DelegateProxyType>
  init<Proxy: DelegateProxyType>(_: Proxy.Type) // expected-warning {{redundant constraint 'P' : 'AnyObject'}}
    where Proxy: GenericDelegateProxy<P, D>,
          Proxy.Parent == P, // expected-warning {{redundant same-type constraint 'GenericDelegateProxy<P, D>.Parent' (aka 'P') == 'P'}}
          Proxy.Delegate == D {} // expected-warning {{redundant same-type constraint 'GenericDelegateProxy<P, D>.Delegate' (aka 'D') == 'D'}}
}

class SomeClass {}
struct SomeStruct {}

class ConcreteDelegateProxy {
  typealias Parent = SomeClass
  typealias Delegate = SomeStruct

  // An even more esoteric edge case. Note that this one I made up; only
  // the first one is relevant for compatibility with RxSwift.
  //
  // Here unfortunately we produce a different result from the GSB, because
  // the hack for keeping both the substituted and original requirement means
  // the substituted requirements become 'P == SomeClass' and 'D == SomeStruct'.
  //
  // The GSB does not constrain P and D in this way and instead produced the
  // following minimized signature:
  //
  // <P, D, Proxy where P == Proxy.[DelegateProxyType]Parent, D == Proxy.[DelegateProxyType]Delegate, Proxy : ConcreteDelegateProxy, Proxy : DelegateProxyType>!

  // CHECK-LABEL: .ConcreteDelegateProxy.init(_:_:_:)@
  // CHECK-NEXT: <P, D, Proxy where P == SomeClass, D == SomeStruct, Proxy : ConcreteDelegateProxy, Proxy : DelegateProxyType, Proxy.[DelegateProxyType]Delegate == SomeStruct, Proxy.[DelegateProxyType]Parent == SomeClass>

  // expected-warning@+2 {{same-type requirement makes generic parameter 'P' non-generic}}
  // expected-warning@+1 {{same-type requirement makes generic parameter 'D' non-generic}}
  init<P, D, Proxy: DelegateProxyType>(_: P, _: D, _: Proxy.Type)
    where Proxy: ConcreteDelegateProxy,
          Proxy.Parent == P,
          Proxy.Delegate == D {}
}

protocol DelegateProxyType {
  associatedtype Parent : AnyObject
  associatedtype Delegate
}
