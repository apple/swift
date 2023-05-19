// RUN: %target-typecheck-verify-swift -disable-objc-attr-requires-foundation-module -enable-objc-interop
// RUN: %target-swift-frontend -disable-objc-attr-requires-foundation-module -debug-generic-signatures -enable-objc-interop -typecheck %s 2>&1 | %FileCheck %s

@objc protocol P1 {}
@objc protocol P2 : P1 {}

class G<X : P1, Y : AnyObject> {}

class C {}

// CHECK-LABEL: Generic signature: <X, Y where X == any P2, Y == C>
extension G where X == P2, Y == C {
  func foo() {}
}
