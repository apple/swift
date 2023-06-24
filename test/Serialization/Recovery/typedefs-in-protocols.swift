// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-sil -o - -emit-module-path %t/Lib.swiftmodule -module-name Lib -I %S/Inputs/custom-modules -disable-objc-attr-requires-foundation-module %s | %FileCheck -check-prefix CHECK-WITNESS-TABLE %s

// RUN: %target-swift-ide-test -source-filename=x -print-module -module-to-print Lib -I %t -I %S/Inputs/custom-modules | %FileCheck %s

// RUN: %target-swift-ide-test -source-filename=x -print-module -module-to-print Lib -I %t -I %S/Inputs/custom-modules -Xcc -DBAD | %FileCheck -check-prefix CHECK-RECOVERY %s

// RUN: %target-swift-frontend -typecheck -I %t -I %S/Inputs/custom-modules -Xcc -DBAD -DTEST -DVERIFY %s -verify

// RUN: %target-swift-frontend -emit-ir -I %t -I %S/Inputs/custom-modules -DTEST %s | %FileCheck -check-prefix CHECK-IR %s
// RUN: %target-swift-frontend -emit-ir -I %t -I %S/Inputs/custom-modules -Xcc -DBAD -DTEST %s | %FileCheck -check-prefix CHECK-IR %s

#if TEST

import Typedefs
import Lib

// CHECK-IR-LABEL: define{{.*}} void @"$s4main19testWitnessDispatch
public func testWitnessDispatch(user: Proto) {
  // The important thing in this CHECK line is the "i32 11", which is the offset
  // for the witness table slot for 'lastMethod()'. If the layout here
  // changes, please check that offset 11 is still correct.
  // CHECK-IR-NOT: ret
  // CHECK-IR: [[SLOT:%.+]] = getelementptr inbounds ptr, ptr {{%.+}}, i32 12
  // CHECK-IR-NOT: ret
  // CHECK-IR: [[RAW_METHOD:%.+]] = load ptr, ptr [[SLOT]]
  // CHECK-IR-NOT: ret
  // CHECK-IR-NOT: ret
  // CHECK-IR: call swiftcc void [[RAW_METHOD]](
  _ = user.lastMethod()
} // CHECK-IR: ret void

// CHECK-IR-LABEL: define{{.*}} void @"$s4main19testGenericDispatch
public func testGenericDispatch<T: Proto>(user: T) {
  // The important thing in this CHECK line is the "i32 11", which is the offset
  // for the witness table slot for 'lastMethod()'. If the layout here
  // changes, please check that offset 11 is still correct.
  // CHECK-IR-NOT: ret
  // CHECK-IR: [[SLOT:%.+]] = getelementptr inbounds ptr, ptr %T.Proto, i32 12
  // CHECK-IR-NOT: ret
  // CHECK-IR: [[RAW_METHOD:%.+]] = load ptr, ptr [[SLOT]]
  // CHECK-IR-NOT: ret
  // CHECK-IR-NOT: ret
  // CHECK-IR: call swiftcc void [[RAW_METHOD]](
  user.lastMethod()
} // CHECK-IR: ret void

#if VERIFY

public class TestImpl : Proto {} // expected-error {{type 'TestImpl' cannot conform to protocol 'Proto' because it has requirements that cannot be satisfied}}

#endif // VERIFY

#else // TEST

import Typedefs

// CHECK-LABEL: protocol Proto {
// CHECK-RECOVERY-LABEL: protocol Proto {
public protocol Proto {
  // CHECK: var unwrappedProp: UnwrappedInt? { get set }
  // CHECK-RECOVERY: var unwrappedProp: Int32?
  var unwrappedProp: UnwrappedInt? { get set }
  // CHECK: var wrappedProp: WrappedInt? { get set }
  // CHECK-RECOVERY: /* placeholder for wrappedProp (vtable entries: 3) */
  var wrappedProp: WrappedInt? { get set }

  // CHECK: func returnsUnwrappedMethod() -> UnwrappedInt
  // CHECK-RECOVERY: func returnsUnwrappedMethod() -> Int32
  func returnsUnwrappedMethod() -> UnwrappedInt
  // CHECK: func returnsWrappedMethod() -> WrappedInt
  // CHECK-RECOVERY: /* placeholder for returnsWrappedMethod() (vtable entries: 1) */
  func returnsWrappedMethod() -> WrappedInt

  // CHECK: subscript(_: WrappedInt) -> () { get }
  // CHECK-RECOVERY: /* placeholder for subscript(_:) (vtable entries: 1) */
  subscript(_: WrappedInt) -> () { get }

  // CHECK: init()
  // CHECK-RECOVERY: init()
  init()

  // CHECK: init(wrapped: WrappedInt)
  // CHECK-RECOVERY: /* placeholder for init(wrapped:) (vtable entries: 1) */
  init(wrapped: WrappedInt)

  func lastMethod()
}
// CHECK: {{^}$}}
// CHECK-RECOVERY: {{^}$}}

// CHECK-LABEL: struct ProtoLibImpl : Proto {
// CHECK-RECOVERY-LABEL: struct ProtoLibImpl : Proto {
public struct ProtoLibImpl : Proto {
  public var unwrappedProp: UnwrappedInt?
  public var wrappedProp: WrappedInt?

  public func returnsUnwrappedMethod() -> UnwrappedInt { fatalError() }
  public func returnsWrappedMethod() -> WrappedInt { fatalError() }

  public subscript(_: WrappedInt) -> () { return () }

  public init() {}
  public init(wrapped: WrappedInt) {}

  public func lastMethod() {}
}
// CHECK: {{^}$}}
// CHECK-RECOVERY: {{^}$}}

// This is mostly to check when changes are necessary for the CHECK-IR lines
// above.
// CHECK-WITNESS-TABLE-LABEL: sil_witness_table{{.*}} ProtoLibImpl: Proto module Lib {
// 0 CHECK-WITNESS-TABLE-NEXT: #Proto.unwrappedProp!getter:
// 1 CHECK-WITNESS-TABLE-NEXT: #Proto.unwrappedProp!setter:
// 2 CHECK-WITNESS-TABLE-NEXT: #Proto.unwrappedProp!modify:
// 3 CHECK-WITNESS-TABLE-NEXT: #Proto.wrappedProp!getter:
// 4 CHECK-WITNESS-TABLE-NEXT: #Proto.wrappedProp!setter:
// 5 CHECK-WITNESS-TABLE-NEXT: #Proto.wrappedProp!modify:
// 6 CHECK-WITNESS-TABLE-NEXT: #Proto.returnsUnwrappedMethod:
// 7 CHECK-WITNESS-TABLE-NEXT: #Proto.returnsWrappedMethod:
// 8 CHECK-WITNESS-TABLE-NEXT: #Proto.subscript!getter:
// 9 CHECK-WITNESS-TABLE-NEXT: #Proto.init!allocator:
// 10 CHECK-WITNESS-TABLE-NEXT: #Proto.init!allocator:
// 11 CHECK-WITNESS-TABLE-NEXT: #Proto.lastMethod:
// CHECK-WITNESS-TABLE: }

#endif // TEST
