// RUN: %target-swift-frontend -primary-file %s -enable-objc-interop -emit-ir | %FileCheck %s

// REQUIRES: CPU=i386 || CPU=x86_64

func foo<T: AnyObject>(_ x: T) -> T { return x }

// CHECK-LABEL: define hidden swiftcc ptr @"$s23generic_class_anyobject3baryyXlyXlF"(ptr %0)
// CHECK:         call swiftcc ptr @"$s23generic_class_anyobject3foo{{[_0-9a-zA-Z]*}}F"
func bar(_ x: AnyObject) -> AnyObject { return foo(x) }
