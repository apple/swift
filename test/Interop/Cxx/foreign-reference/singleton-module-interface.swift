// RUN: %target-swift-ide-test -print-module -module-to-print=Singleton -I %S/Inputs -source-filename=x -enable-experimental-cxx-interop | %FileCheck %s

// CHECK: class DeletedDtor {
// CHECK-NOT: init
// CHECK:   func test() -> Int32
// CHECK:   func testMutable() -> Int32
// CHECK:   class func create() -> DeletedDtor
// CHECK:   var value: Int32
// CHECK: }
// CHECK: func mutateIt(_ x: DeletedDtor)

// CHECK: class PrivateDtor {
// CHECK-NOT: init
// CHECK:   func test() -> Int32
// CHECK:   func testMutable() -> Int32
// CHECK:   class func create() -> PrivateDtor
// CHECK:   var value: Int32
// CHECK: }
// CHECK: func mutateIt(_ x: PrivateDtor)

// CHECK: class DeletedSpecialMembers {
// CHECK-NOT: init
// CHECK:   func test() -> Int32
// CHECK:   func testMutable() -> Int32
// CHECK:   class func create() -> DeletedSpecialMembers
// CHECK:   var value: Int32
// CHECK: }
// CHECK: func mutateIt(_ x: DeletedSpecialMembers)

// CHECK: class PrivateSpecialMembers {
// CHECK-NOT: init
// CHECK:   func test() -> Int32
// CHECK:   func testMutable() -> Int32
// CHECK:   class func create() -> PrivateSpecialMembers
// CHECK:   var value: Int32
// CHECK: }
// CHECK: func mutateIt(_ x: PrivateSpecialMembers)
