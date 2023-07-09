// RUN: %target-swift-ide-test -print-module -module-to-print=TypeClassification -I %S/Inputs -source-filename=x -enable-experimental-cxx-interop | %FileCheck %s
// RUN: %target-swift-ide-test -print-module -skip-unsafe-cxx-methods -module-to-print=TypeClassification -I %S/Inputs -source-filename=x -enable-experimental-cxx-interop | %FileCheck %s -check-prefix=CHECK-SKIP-UNSAFE

// Make sure we don't import objects that we can't copy or destroy.
// CHECK-NOT: StructWithPrivateDefaultedCopyConstructor
// CHECK-NOT: StructWithInheritedPrivateDefaultedCopyConstructor
// CHECK-NOT: StructWithSubobjectPrivateDefaultedCopyConstructor
// CHECK-NOT: StructNonCopyableTriviallyMovable
// CHECK-NOT: StructWithPointerNonCopyableTriviallyMovable
// CHECK-NOT: StructWithPointerNonCopyableTriviallyMovableField
// CHECK-NOT: StructNonCopyableNonMovable
// CHECK-NOT: StructWithMoveConstructor
// CHECK-NOT: StructWithInheritedMoveConstructor
// CHECK-NOT: StructWithSubobjectMoveConstructor
// CHECK-NOT: StructWithMoveAssignment
// CHECK-NOT: StructWithInheritedMoveAssignment
// CHECK-NOT: StructWithSubobjectMoveAssignment
// CHECK-NOT: StructWithPrivateDefaultedDestructor
// CHECK-NOT: StructWithInheritedPrivateDefaultedDestructor
// CHECK-NOT: StructWithSubobjectPrivateDefaultedDestructor
// CHECK-NOT: StructWithDeletedCopyConstructor
// CHECK-NOT: StructWithMoveConstructorAndDeletedCopyConstructor
// CHECK-NOT: StructWithDeletedDestructor
// CHECK-NOT: StructWithInheritedDeletedDestructor
// CHECK-NOT: StructWithSubobjectDeletedDestructor

// CHECK: struct Iterator {
// CHECK: }

// CHECK: struct HasMethodThatReturnsIterator {
// CHECK:   func __getIteratorUnsafe() -> Iterator
// CHECK-SKIP-UNSAFE-NOT: func __getIteratorUnsafe() -> Iterator
// CHECK: }

// CHECK: struct IteratorBox {
// CHECK: }

// CHECK: struct HasMethodThatReturnsIteratorBox {
// CHECK:   func __getIteratorBoxUnsafe() -> IteratorBox
// CHECK-SKIP-UNSAFE-NOT: func __getIteratorBoxUnsafe() -> IteratorBox
// CHECK: }
