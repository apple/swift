// RUN: %target-swift-frontend %s -emit-ir -g -module-name inout -o %t.ll
// RUN: cat %t.ll | %FileCheck %s
// RUN: cat %t.ll | %FileCheck %s --check-prefix=PROMO-CHECK
// RUN: cat %t.ll | %FileCheck %s --check-prefix=FOO-CHECK

// LValues are direct values, too. They are reference types, though.

func Close(_ fn: () -> Int64) { fn() }
typealias MyFloat = Float

// CHECK: define hidden swiftcc void @"$s5inout13modifyFooHeap{{[_0-9a-zA-Z]*}}F"
// CHECK: %[[ALLOCA:.*]] = alloca ptr
// CHECK: call void @llvm.dbg.declare(metadata
// CHECK-SAME:                        %[[ALLOCA]], metadata ![[A:[0-9]+]]

// Closure with promoted capture.
// PROMO-CHECK: define {{.*}}@"$s5inout13modifyFooHeapyys5Int64Vz_SftFADyXEfU_"
// PROMO-CHECK: call void @llvm.dbg.declare(metadata ptr %
// PROMO-CHECK-SAME:   metadata ![[A1:[0-9]+]], metadata !DIExpression(DW_OP_deref))

// PROMO-CHECK: ![[INT:.*]] = !DICompositeType({{.*}}identifier: "$ss5Int64VD"
// PROMO-CHECK: ![[A1]] = !DILocalVariable(name: "a", arg: 1,{{.*}} type: ![[INT]]
func modifyFooHeap(_ a: inout Int64,
  // CHECK-DAG: ![[A]] = !DILocalVariable(name: "a", arg: 1{{.*}} line: [[@LINE-1]],{{.*}} type: ![[RINT:[0-9]+]]
  // CHECK-DAG: ![[RINT]] = !DICompositeType({{.*}}identifier: "$ss5Int64VD"
                   _ b: MyFloat)
{
    let b = b
    if (b > 2.71) {
      a = a + 12// Set breakpoint here
    }

    // Close over the variable to disable promotion of the inout shadow.
    Close({ a })
}

// Inout reference type.
// FOO-CHECK: define {{.*}}@"$s5inout9modifyFooyys5Int64Vz_SftF"
// FOO-CHECK: call void @llvm.dbg.declare(metadata ptr %
// FOO-CHECK-SAME: metadata ![[U:[0-9]+]], metadata !DIExpression(DW_OP_deref))
func modifyFoo(_ u: inout Int64,
// FOO-CHECK-DAG: !DILocalVariable(name: "v", arg: 2{{.*}} line: [[@LINE+3]],{{.*}} type: ![[LET_MYFLOAT:[0-9]+]]
// FOO-CHECK-DAG: [[U]] = !DILocalVariable(name: "u", arg: 1,{{.*}} line: [[@LINE-2]],{{.*}} type: ![[RINT:[0-9]+]]
// FOO-CHECK-DAG: ![[RINT]] = !DICompositeType({{.*}}identifier: "$ss5Int64VD"
  _ v: MyFloat)
// FOO-CHECK-DAG: ![[LET_MYFLOAT]] = !DIDerivedType(tag: DW_TAG_const_type, baseType: ![[MYFLOAT:[0-9]+]])
// FOO-CHECK-DAG: ![[MYFLOAT]] = !DIDerivedType(tag: DW_TAG_typedef, name: "$s5inout7MyFloataD",{{.*}} baseType: ![[FLOAT:[0-9]+]]
// FOO-CHECK-DAG: ![[FLOAT]] = !DICompositeType({{.*}}identifier: "$sSfD"
{
    if (v > 2.71) {
      u = u - 41
    }
}

func main() -> Int64 {
    var c = 11 as Int64
    modifyFoo(&c, 3.14)

    var d = 64 as Int64
    modifyFooHeap(&d, 1.41)
    return 0
}

main()

