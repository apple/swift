// RUN: %target-typecheck-verify-swift -warn-redundant-requirements
// RUN: not %target-swift-frontend -typecheck -debug-generic-signatures %s 2>&1 | %FileCheck %s

struct S {}
class C {}

struct G1<T : AnyObject> {}

// CHECK: ExtensionDecl line={{.*}} base=G1
// CHECK-NEXT: Generic signature: <T>
extension G1 where T == S {}
// expected-error@-1 {{no type for 'T' can satisfy both 'T : AnyObject' and 'T == S'}}

// CHECK: ExtensionDecl line={{.*}} base=G1
// CHECK-NEXT: Generic signature: <T where T == C>
extension G1 where T == C {} // expected-warning {{redundant constraint 'T' : 'AnyObject'}}

struct G2<U> {}

// CHECK: ExtensionDecl line={{.*}} base=G2
// CHECK-NEXT: Generic signature: <U>
extension G2 where U == S, U : AnyObject {}
// expected-error@-1 {{no type for 'U' can satisfy both 'U : AnyObject' and 'U == S'}}

// CHECK: ExtensionDecl line={{.*}} base=G2
// CHECK-NEXT: Generic signature: <U where U == C>
extension G2 where U == C, U : AnyObject {}
// expected-warning@-1 {{redundant constraint 'U' : 'AnyObject'}}

// CHECK: ExtensionDecl line={{.*}} base=G2
// CHECK-NEXT: Generic signature: <U where U : C>
extension G2 where U : C, U : AnyObject {}
// expected-warning@-1 {{redundant constraint 'U' : 'AnyObject'}}

// Explicit AnyObject conformance vs derived same-type
protocol P {
  associatedtype A where A == C
}

// CHECK: .explicitAnyObjectIsRedundant@
// CHECK-NEXT: Generic signature: <T where T : P>
func explicitAnyObjectIsRedundant<T : P>(_: T) where T.A : AnyObject {}
// expected-warning@-1 {{redundant constraint 'T.A' : 'AnyObject'}}
