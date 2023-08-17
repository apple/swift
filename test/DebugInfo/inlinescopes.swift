// RUN: %empty-directory(%t)
// RUN: echo "public var x = Int64()" \
// RUN:   | %target-swift-frontend -module-name FooBar -emit-module -o %t -
// RUN: %target-swift-frontend %s -O -I %t -emit-ir -g -o %t.ll
// RUN: %FileCheck %s < %t.ll
// RUN: %FileCheck %s -check-prefix=TRANSPARENT-CHECK < %t.ll

// CHECK: define{{( dllexport)?}}{{( protected)?( signext)?}} i32 @main{{.*}}
// CHECK: call swiftcc i64 @"$s4main8noinlineys5Int64VADF"(i64 %{{.*}})
// CHECK-SAME: !dbg ![[CALL:.*]]
// CHECK-DAG: ![[TOPLEVEL:.*]] = !DIFile(filename: "{{.*}}inlinescopes.swift"

import FooBar

@inline(never)
func use(_ x: Int64) -> Int64 { return x }

@inline(never)
func noinline(_ x: Int64) -> Int64 { return use(x) }

@_transparent
func transparent(_ x: Int64) -> Int64 { return noinline(x) }


@inline(__always)
func inlined(_ x: Int64) -> Int64 {
  let result = transparent(x)
// CHECK-DAG: ![[CALL]] = !DILocation(line: [[@LINE-1]], column: {{.*}}, scope: ![[INLINED1:.*]], inlinedAt: ![[INLINEDAT:.*]])
// CHECK-DAG: ![[INLINEDAT]] = distinct !DILocation({{.*}}scope: ![[INLINEDAT1:[0-9]+]]
// CHECK-DAG: ![[INLINED1]] = distinct !DILexicalBlock(scope: ![[INLINED:[0-9]+]]
// Check if the inlined and removed function still has the correct linkage name.
// CHECK-DAG: ![[INLINED]] = distinct !DISubprogram(name: "inlined", linkageName: "$s4main7inlinedys5Int64VADF"
// TRANSPARENT-CHECK-NOT: !DISubprogram(name: "transparent"
  return result
}
// CHECK-DAG: !DIGlobalVariable(name: "y",{{.*}} file: ![[TOPLEVEL]],{{.*}} line: [[@LINE+1]]
public let y = inlined(x)
