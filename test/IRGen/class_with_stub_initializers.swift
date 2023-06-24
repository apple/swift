// RUN: %empty-directory(%t)
// RUN: %build-irgen-test-overlays
// RUN: %target-swift-frontend(mock-sdk: -sdk %S/Inputs -I %t) %s -emit-ir | %FileCheck %s

// REQUIRES: objc_interop

import Foundation
// CHECK: @"$s28class_with_stub_initializers3FooCMf" =
// CHECK-NOT: s28class_with_stub_initializers3FooCACycfc
// -- The init() stub should get no vtable entry
// CHECK: s28class_with_stub_initializers3FooC1xACs5Int64V_tcfC
// CHECK-NOT: s28class_with_stub_initializers3FooCACycfc
// -- The init() stub should get no vtable entry
// CHECK:       {{^(@|define)}}
class Foo: NSObject {
  init(x: Int64) { super.init() }
}
