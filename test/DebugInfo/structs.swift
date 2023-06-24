// RUN: %target-swift-frontend -primary-file %s -emit-ir -g -o - | %FileCheck %s

// Capture the pointer size from type Int
// CHECK: %TSi = type <{ i[[PTRSIZE:[0-9]+]] }>

struct A {
  var fn : () -> ()
}

func test(_ x : A) {
  var _ = x
}
// CHECK:    define hidden {{.*}}void @"$s7structs4test{{[_0-9a-zA-Z]*}}F"
// CHECK: [[X_DBG:%.*]] = alloca
// CHECK: call void @llvm.dbg.declare(metadata ptr [[X_DBG]], metadata [[X_MD:!.*]], metadata

// A class is represented by a pointer, so B's total size should be PTRSIZE.
// CHECK: !DICompositeType(tag: DW_TAG_structure_type, name: "B",{{.*}}size: [[PTRSIZE]]

// CHECK: ![[A:.*]] = !DICompositeType(tag: DW_TAG_structure_type, name: "A",
// CHECK-SAME:                         identifier

class C {
  var lots_of_extra_storage: (Int, Int, Int) = (1, 2, 3)
  var member: C = C()
}

// CHECK: [[X_MD]] = !DILocalVariable(name: "x", arg: 1
// CHECK-SAME:                        type: ![[LET_A:[0-9]+]]
// CHECK: ![[LET_A]] = !DIDerivedType(tag: DW_TAG_const_type, baseType: ![[A]])

struct B {
  var c : C
}

let b = B(c: C())
