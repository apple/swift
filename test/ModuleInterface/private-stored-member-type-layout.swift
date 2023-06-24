// RUN: %empty-directory(%t)

// Check that importing this module creates the right types

// RUN: %target-swift-frontend -emit-module-interface-path %t/private-stored-members.swiftinterface -module-name PrivateStoredMembers -emit-module -o %t/PrivateStoredMembers.swiftmodule %S/private-stored-members.swift
// RUN: %target-swift-frontend -emit-ir %s -I %t 2>&1 -DSHOULD_IMPORT | %FileCheck %s --check-prefix CHECK-EXEC --check-prefix CHECK

// Check that the types are also correct when importing a module created from an interface

// RUN: %target-swift-frontend -emit-module -o %t/PrivateStoredMembers.swiftmodule -module-name PrivateStoredMembers %t/private-stored-members.swiftinterface -disable-objc-attr-requires-foundation-module
// RUN: %target-swift-frontend -emit-ir %s -I %t 2>&1 -DSHOULD_IMPORT | %FileCheck %s --check-prefix CHECK-EXEC --check-prefix CHECK

// Check the types generated when the source file is the primary file, and ensure they're the same layout.

// RUN: %target-swift-frontend -emit-ir %S/private-stored-members.swift %s 2>&1 -module-name main | %FileCheck %s --check-prefix CHECK-MAIN --check-prefix CHECK-EXEC

// These two appear out-of-order between run lines

// CHECK-DAG: [[MYSTRUCT:%T20PrivateStoredMembers8MyStructV]] = type <{ %Ts5Int64V, %TSb, [7 x i8], %Ts5Int64V, %TSb, [7 x i8], %Ts5Int64V, %TSb, [7 x i8], %Ts5Int64V, %TSb, [7 x i8], %Ts5Int64V }>

// CHECK-MAIN-DAG: [[MYCLASS:%T4main7MyClassC]] = type <{ %swift.refcounted, %Ts5Int64V, %TSb, [7 x i8], %Ts5Int64V, %TSb, [7 x i8], %Ts5Int64V, %TSb, [7 x i8], %Ts5Int64V, %TSb, [7 x i8], %Ts5Int64V }>
// CHECK-MAIN-DAG: [[MYSTRUCT:%T4main8MyStructV]] = type <{ %Ts5Int64V, %TSb, [7 x i8], %Ts5Int64V, %TSb, [7 x i8], %Ts5Int64V, %TSb, [7 x i8], %Ts5Int64V, %TSb, [7 x i8], %Ts5Int64V }>

#if SHOULD_IMPORT
import PrivateStoredMembers
#endif

// CHECK-EXEC: swiftcc void @"$s{{[^ ]+}}8makeUseryyF"() #0 {
public func makeUser() {
  let ptr = UnsafeMutablePointer<MyStruct>.allocate(capacity: 1)
  // CHECK-EXEC: %.publicEndVar = getelementptr inbounds [[MYSTRUCT]], ptr %{{[0-9]+}}, i32 0, i32 [[PUBLIC_END_VAR_IDX:12]]
  // CHECK-EXEC: %.publicEndVar._value = getelementptr inbounds %Ts5Int64V, ptr %.publicEndVar, i32 0, i32 0
  // CHECK-EXEC: store i64 4, ptr %.publicEndVar._value
  ptr.pointee.publicEndVar = 4

  // CHECK-EXEC: %.publicEndVar1 = getelementptr inbounds [[MYSTRUCT]], ptr %{{[0-9]+}}, i32 0, i32 [[PUBLIC_END_VAR_IDX]]
  // CHECK-EXEC: %.publicEndVar1._value = getelementptr inbounds %Ts5Int64V, ptr %.publicEndVar1, i32 0, i32 0
  // CHECK-EXEC: [[PUBLIC_END_VAR_LOAD:%[0-9]+]] = load i64, ptr %.publicEndVar1._value, align 8

  // CHECK-EXEC: %.publicVar = getelementptr inbounds [[MYSTRUCT]], ptr %{{[0-9]+}}, i32 0, i32 0
  // CHECK-EXEC: %.publicVar._value = getelementptr inbounds %Ts5Int64V, ptr %.publicVar, i32 0, i32 0
  // CHECK-EXEC: store i64 [[PUBLIC_END_VAR_LOAD]], ptr %.publicVar._value, align 8
  ptr.pointee.publicVar = ptr.pointee.publicEndVar
  ptr.deallocate()

  // CHECK-EXEC: %[[MYCLASS_INIT:[0-9]+]] = call swiftcc ptr @"$s{{[^ ]+}}7MyClassCACycfC"(ptr swiftself %{{[0-9]+}})
  let myClass = MyClass()

  // These are uninteresting as they just call into the standard getter and setter.
  myClass.publicEndVar = 4
  myClass.publicVar = myClass.publicEndVar
}
