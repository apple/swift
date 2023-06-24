// RUN: %target-swift-frontend %use_no_opaque_pointers -primary-file %s -enable-objc-interop -emit-ir | %FileCheck %s
// RUN: %target-swift-frontend -primary-file %s -enable-objc-interop -emit-ir

// REQUIRES: CPU=i386 || CPU=x86_64

func foo<T: AnyObject>(_ x: T) -> T { return x }

// CHECK-LABEL: define hidden swiftcc %objc_object* @"$s23generic_class_anyobject3baryyXlyXlF"(%objc_object* %0)
// CHECK:         call swiftcc %objc_object* @"$s23generic_class_anyobject3foo{{[_0-9a-zA-Z]*}}F"
func bar(_ x: AnyObject) -> AnyObject { return foo(x) }
