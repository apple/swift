// RUN: %empty-directory(%t)
// RUN: %target-swift-emit-module-interface(%t.swiftinterface) %s -module-name Test -enable-experimental-feature SuppressedAssociatedTypes
// RUN: %target-swift-typecheck-module-from-interface(%t.swiftinterface) -module-name Test
// RUN: %FileCheck %s < %t.swiftinterface

// CHECK: #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK: public protocol P : ~Copyable {
// CHECK:   associatedtype A : ~Copyable
// CHECK: }
// CHECK: #else
// CHECK: public protocol P {
// CHECK:   associatedtype A
// CHECK: }
// CHECK: #endif
public protocol P: ~Copyable {
  associatedtype A: ~Copyable
}

// CHECK: #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK: public struct X<T> : ~Swift.Copyable where T : ~Copyable {
// CHECK: }
// CHECK: #else
// CHECK: public struct X<T> {
// CHECK: }
// CHECK: #endif
public struct X<T: ~Copyable>: ~Copyable { }

// CHECK: #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK:      extension Test.X : Swift.Copyable where T : Swift.Copyable {
// CHECK-NEXT:   func f()
// CHECK:      }
// CHECK: #else
// CHECK:      extension Test.X {
// CHECK-NEXT:   func f()
// CHECK:      }
extension X: Copyable where T: Copyable {
  public func f() { }
}

// CHECK: #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK: extension Test.X where T : ~Copyable {
// CHECK:   public func g(other: borrowing T)
// CHECK: }
// CHECK: #else
// CHECK: extension Test.X {
// CHECK:   public func g(other: borrowing T)
// CHECK: }
// CHECK: #endif
extension X where T: ~Copyable {
  public func g(other: borrowing T) { }
}

// CHECK: #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK: public enum Y<T> : ~Swift.Copyable where T : ~Copyable {
// CHECK:   case none
// CHECK:   case some(T)
// CHECK: }
// CHECK: #else
// CHECK: public enum Y<T> {
// CHECK:   case none
// CHECK:   case some(T)
// CHECK: }
public enum Y<T: ~Copyable>: ~Copyable {
  case none
  case some(T)
}

extension Y: Copyable where T: Copyable { }
