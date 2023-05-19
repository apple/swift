// RUN: %target-typecheck-verify-swift -warn-redundant-requirements
// RUN: %target-swift-frontend -typecheck %s -debug-generic-signatures 2>&1 | %FileCheck %s

public struct G<T: P1, U: P1> {
  // CHECK-LABEL: .f1()@
  // CHECK-NEXT: Generic signature: <T, U where T : P1, T == U>
  public func f1() where T == U {} // expected-warning {{redundant conformance constraint 'U' : 'P1'}}

  // CHECK-LABEL: .f2()@
  // CHECK-NEXT: Generic signature: <T, U where T : P1, T == U>
  public func f2() where T == U, T.A.B == T {} // expected-warning {{redundant conformance constraint 'U' : 'P1'}}
  // expected-warning@-1 {{redundant same-type constraint 'T.A.B' == 'T'}}

  // CHECK-LABEL: .f3()@
  // CHECK-NEXT: Generic signature: <T, U where T : P1, T == U>
  public func f3() where T.A.B == T, T == U {} // expected-warning {{redundant conformance constraint 'U' : 'P1'}}
  // expected-warning@-1 {{redundant same-type constraint 'T.A.B' == 'T'}}

  // CHECK-LABEL: .f4()@
  // CHECK-NEXT: Generic signature: <T, U where T : P1, T == U>
  public func f4() where U.A.B == U, T == U {} // expected-warning {{redundant conformance constraint 'U' : 'P1'}}
  // expected-warning@-1 {{redundant same-type constraint 'U.A.B' == 'U'}}
}

public protocol P1 {
  associatedtype A: P2 where A.B == Self
}

public protocol P2 {
  associatedtype B: P1
}
