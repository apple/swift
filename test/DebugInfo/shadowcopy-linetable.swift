// RUN: %target-swift-frontend %s -emit-ir -g -o - | %FileCheck %s

func markUsed<T>(_ t: T) {}

func foo(_ x: inout Int64) {
  // Make sure the shadow copy is being made in the prologue or (at
  // line 0), but the code to load the value from the inout storage is
  // not.
  // CHECK: define hidden swiftcc void @"$s4main3fooyys5Int64VzF"
  // CHECK: %[[X:.*]] = alloca ptr, align {{(4|8)}}
  // CHECK-NEXT: call void @llvm.dbg.declare
  // CHECK-NEXT: call void @llvm.memset.{{.*}}(ptr align {{(4|8)}} %[[X]], i8 0
  // CHECK: store ptr %0, ptr %[[X]], align {{(4|8)}}
  // CHECK-SAME: !dbg ![[LOC0:.*]]
  // CHECK-NEXT: getelementptr inbounds %Ts5Int64V, ptr %0, i32 0, i32 0,
  // CHECK-SAME: !dbg ![[LOC0]]
  // CHECK: ![[LOC0]] = !DILocation(line: 0,
  // CHECK: !DILocation(line: [[@LINE+1]],
  x = x + 2
}

func main() {
  var x : Int64 = 1
  foo(&x)
  markUsed("break here to see \(x)")
}

main()
