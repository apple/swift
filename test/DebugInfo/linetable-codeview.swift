// RUN: %swiftc_driver %s -g -debug-info-format=codeview -emit-ir -o - | %FileCheck %s
// REQUIRES: optimized_stdlib

func markUsed<T>(_ t: T) {}
func arithmetic(_ a: Int64, _ b: Int64) {
  markUsed(a + b)             // line 6
  markUsed(a / b)             // line 7
}
struct SimpleStruct { // NOTE: Classes do not work in Windows yet.
  var myVal1: Float
  var myVal2: Float
  func sum(myArg: Float) {    // line 12
    markUsed(myVal1 + myVal2 + myArg)
  }
}
func myLoop() {
  for index in 0...3 {        // line 17
    markUsed(index)           // line 18
  }
}
func mySwitch(_ a: Int64) {
  switch a {
  case 0:
    markUsed(a-1)             // line 24
  default: do {
      markUsed(a+1)
    }                         // line 27
  }
}
func foo() {
  var myArray: [Int64] = []   // line 31
}

// func arithmetic(_ a: Int64, _ b: Int64)
  // CHECK: define {{.*}} @"$s4main10arithmeticyys5Int64V_ADtF"(i64 %0, i64 %1)
  // CHECK: call { i64, i1 } @llvm.sadd.with.overflow.i64({{.*}}), !dbg ![[ADD:[0-9]+]]
  // NOTE: The division will emit an ``unreachable`` instruction sandwiched
  //       between other instructions for the division. We want to make sure
  //       all instructions from the division have the same debug location and
  //       are contiguous.
  // CHECK: call {{.*}} @"$ss17_assertionFailure__4file4line5flagss5NeverOs12StaticStringV_A2HSus6UInt32VtF"{{.*}}, !dbg ![[DIV:[0-9]+]]
  // CHECK-NEXT: unreachable, !dbg ![[DIV]]
  // CHECK: sdiv i64 %0, %1, !dbg ![[DIV]]
  // CHECK: call void @llvm.trap(), !dbg ![[INLINEDADD:[0-9]+]]
  // CHECK-NEXT: unreachable, !dbg ![[INLINEDADD]]

// func sum(myArg: Float)
  // CHECK: define {{.*}} @"$s4main12SimpleStructV3sum5myArgySf_tF"{{.*}} !dbg ![[SUM:[0-9]+]]
  // NOTE: The point of this test is to trigger IRGenSIL::emitShadowCopy()
  //       and IRGenSIL::emitShadowCopyIfNeeded(). It may be worthwhile to
  //       simplify this testcase.
  // CHECK: store float %0, ptr %myArg.debug, {{.*}}, !dbg ![[PROLOGUE:[0-9]+]]
  // CHECK: store float {{.*}}, ptr %self.debug.myVal1._value, {{.*}}, !dbg ![[PROLOGUE]]

// func myLoop() {
  // CHECK: define {{.*}} @"$s4main6myLoopyyF"
  // CHECK: call void @llvm.dbg.declare(metadata ptr %index.debug, {{.*}}), !dbg ![[FORLOOP:[0-9]+]]
  // CHECK: phi i64 [ %{{.[0-9]+}}, %{{.[0-9]+}} ], !dbg ![[FORLOOP]]
  // CHECK: call {{.*}} @"$s4main8markUsedyyxlF"{{.*}}, !dbg ![[FORBODY:[0-9]+]]
  // CHECK: ret void

// func mySwitch(_ a: Int64)
  // CHECK: call { i64, i1 } @llvm.ssub.with.overflow.i64{{.*}}
  // CHECK: br label %[[RETLABEL:[0-9]+]], !dbg ![[CASE:[0-9]+]]
  // CHECK: call { i64, i1 } @llvm.sadd.with.overflow.i64{{.*}}
  // CHECK: br label %[[RETLABEL]], !dbg ![[DEFAULTCLEANUP:[0-9]+]]
  // CHECK: [[RETLABEL]]:
  // CHECK-NEXT: ret void

// func foo()
  // CHECK: define {{.*}} @"$s4main3fooyyF"
  // CHECK: %[[MYARRAY:.*]] = alloca
  // CHECK: call void @llvm.dbg.declare(metadata ptr %[[MYARRAY]],
  // CHECK-SAME: !dbg ![[ARRAY:[0-9]+]]
  // CHECK: call swiftcc { {{.*}} } @"${{.*}}_allocateUninitializedArray{{.*}}"
  // CHECK-SAME: !dbg ![[ARRAY_ALLOC:[0-9]+]]
  // CHECK: ret void

// CHECK-DAG: ![[ADD]] = !DILocation(line: 6, scope:
// CHECK-DAG: ![[DIV]] = !DILocation(line: 7, scope:
// FIXME: The location of ``@llvm.trap`` should be in Integers.swift.gyb
//        instead of being artificial.
// CHECK: ![[INLINEDADD]] = !DILocation(line: 0, scope: ![[FAILURE_FUNC:[0-9]+]], inlinedAt: ![[INLINELOC:[0-9]+]]
// CHECK-DAG: !{{.*}} = distinct !DISubprogram(name: "Swift runtime failure: arithmetic overflow", scope: {{.*}}, flags: DIFlagArtificial, spFlags: DISPFlagDefinition, {{.*}})
// CHECK-DAG: ![[INLINELOC]] = !DILocation(line: 0, scope: !{{[0-9]+}}, inlinedAt: ![[ADD]]

// NOTE: These prologue instructions are given artificial line locations for
//       LLDB, but for CodeView they should have the location of the function
//       to keep the linetables contiguous.
// CHECK-DAG: ![[SUM]] = distinct !DISubprogram(name: "sum", linkageName: "$s4main12SimpleStructV3sum5myArgySf_tF"
// CHECK-DAG: ![[PROLOGUE]] = !DILocation(line: 12, scope: ![[SUM]])
// CHECK-DAG: ![[FORLOOP]] = !DILocation(line: 17, scope:
// CHECK-DAG: ![[FORBODY]] = !DILocation(line: 18, scope:
// CHECK-DAG: ![[CASE]] = !DILocation(line: 24, scope:
// CHECK-DAG: ![[DEFAULTCLEANUP]] = !DILocation(line: 27, scope:
// CHECK-DAG: ![[ARRAY]] = !DILocation(line: 31, scope:
// CHECK-DAG: ![[ARRAY_ALLOC]] = !DILocation(line: 31, scope:
