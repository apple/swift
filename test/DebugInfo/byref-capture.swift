// RUN: %target-swift-frontend %s -emit-ir -g -o - | %FileCheck %s

func makeIncrementor(_ inc : Int64) -> () -> Int64
{
  var sum : Int64 = 0
  // CHECK: define {{.*}}5inner
  func inner() -> Int64 {
    // CHECK: call void @llvm.dbg.declare(metadata ptr
    // CHECK-SAME:                        metadata ![[SUM_CAPTURE:[0-9]+]],
    // CHECK-SAME:                        metadata !DIExpression(DW_OP_deref))
    // CHECK: ![[INOUTTY:[0-9]+]] = !DICompositeType({{.*}}identifier: "$ss5Int64VD"
    // CHECK: ![[SUM_CAPTURE]] = !DILocalVariable(name: "sum", arg: 1,
    // CHECK-SAME:     line: [[@LINE-8]], type: ![[INOUTTY]]
    sum += inc
    return sum
  }
  return inner
}

var incrementor = makeIncrementor(5)
incrementor()
