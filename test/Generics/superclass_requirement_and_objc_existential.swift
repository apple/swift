// RUN: %target-typecheck-verify-swift -disable-objc-attr-requires-foundation-module -warn-redundant-requirements
// RUN: not %target-swift-frontend -typecheck %s -debug-generic-signatures -disable-objc-attr-requires-foundation-module 2>&1 | %FileCheck %s

// REQUIRES: objc_interop

class C {}
@objc protocol P1 {}
protocol P2 {}
protocol P3 : AnyObject {}

protocol Q {
  associatedtype A
}

/// This is allowed, even though it's not fully sound.

// CHECK-LABEL: .f1@
// CHECK-NEXT: Generic signature: <T where T : Q, T.[Q]A == any C & P1>
func f1<T : Q>(_: T) where T.A : C, T.A == any (C & P1) {}
// expected-warning@-1 {{redundant superclass constraint 'T.A' : 'C'}}

/// These are not allowed.

// CHECK-LABEL: .f2@
// CHECK-NEXT: Generic signature: <T where T : Q>
func f2<T : Q>(_: T) where T.A : C, T.A == any P1 {}
// expected-error@-1 {{no type for 'T.A' can satisfy both 'T.A : C' and 'T.A == any P1'}}

// CHECK-LABEL: .f3@
// CHECK-NEXT: Generic signature: <T where T : Q>
func f3<T : Q>(_: T) where T.A : C, T.A == any (C & P2) {}
// expected-error@-1 {{no type for 'T.A' can satisfy both 'T.A : C' and 'T.A == any C & P2'}}
// expected-error@-2 {{no type for 'T.A' can satisfy both 'T.A : _NativeClass' and 'T.A == any C & P2'}}

// CHECK-LABEL: .f4@
// CHECK-NEXT: Generic signature: <T where T : Q>
func f4<T : Q>(_: T) where T.A : C, T.A == any (C & P3) {}
// expected-error@-1 {{no type for 'T.A' can satisfy both 'T.A : C' and 'T.A == any C & P3'}}
// expected-error@-2 {{no type for 'T.A' can satisfy both 'T.A : _NativeClass' and 'T.A == any C & P3'}}

