// RUN: %target-swift-ide-test -print-module -module-to-print=Nullable -I %S/Inputs -source-filename=x -enable-experimental-cxx-interop | %FileCheck %s

// CHECK: class Empty {
// CHECK:   func test() -> Int32
// CHECK:   class func create() -> Empty!
// CHECK: }
// CHECK: func mutateIt(_: Empty)

// CHECK: class IntPair {
// CHECK:   func test() -> Int32
// CHECK:   class func create() -> IntPair!
// CHECK:   var a: Int32
// CHECK:   var b: Int32
// CHECK: }
// CHECK: func mutateIt(_ x: IntPair!)
