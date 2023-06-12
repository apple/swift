// RUN: %target-swift-ide-test -print-module -module-to-print=AnonymousUnionPartlyInvalid -I %S/Inputs -source-filename=x -enable-experimental-cxx-interop -enable-objc-interop | %FileCheck %s

// CHECK: class C {
// CHECK-NEXT: }
// CHECK-NEXT: struct S {
// CHECK-NEXT:   @available(*, deprecated, message
// CHECK-NEXT:   init()
// CHECK-NEXT:   mutating func f() -> Int32
// CHECK-NEXT: }
// CHECK-NEXT: func getSPtr() -> UnsafeMutablePointer<S>!
