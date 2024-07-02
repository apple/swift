// RUN: %target-swift-ide-test -print-module -module-to-print=MoveOnly -I %S/Inputs -source-filename=x -enable-experimental-cxx-interop | %FileCheck %s

// CHECK: class MoveOnly {
// CHECK-NOT: init
// CHECK:   func test() -> Int32
// CHECK:   func testMutable() -> Int32
// CHECK:   class func create() -> MoveOnly
// CHECK: }
// CHECK-NOT: func moveIntoResult(_ x: MoveOnly) -> MoveOnly

// CHECK: class HasMoveOnlyChild {
// CHECK-NOT: init
// CHECK-NOT: var child: MoveOnly
// CHECK:   class func create() -> HasMoveOnlyChild
// CHECK: }
// CHECK-NOT: func moveIntoResult(_ x: HasMoveOnlyChild) -> HasMoveOnlyChild

// CHECK: class PrivateCopyCtor {
// CHECK-NOT: init
// CHECK:   func test() -> Int32
// CHECK:   func testMutable() -> Int32
// CHECK:   class func create() -> PrivateCopyCtor
// CHECK: }
// CHECK-NOT: func moveIntoResult(_ x: PrivateCopyCtor) -> PrivateCopyCtor

// CHECK: class BadCopyCtor {
// CHECK-NOT: init
// CHECK:   func test() -> Int32
// CHECK:   func testMutable() -> Int32
// CHECK:   class func create() -> BadCopyCtor
// CHECK: }
// CHECK-NOT: func moveIntoResult(_ x: BadCopyCtor) -> BadCopyCtor
